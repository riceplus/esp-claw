/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight HTTP Live Streaming (HLS) reader IO for ESP-GMF.
 *
 * The IO runs in synchronous mode: the GMF io-process task calls
 * `acquire_read`, which downloads TS/AAC segments on demand and serves them
 * through an internal chunk cursor. Because `io_process_read` requests only
 * `io_size` bytes per call, the segment download is stateful: the whole
 * segment is buffered in a heap buffer and consumed across multiple
 * `acquire_read` calls.
 *
 * Live streams are handled by tracking the media sequence number; once the
 * segment window is exhausted, the playlist is re-fetched and only new
 * segments (sequence >= next expected) are downloaded.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "esp_gmf_oal_mem.h"
#include "esp_gmf_obj.h"
#include "audio_hls_io.h"

#define HLS_TAG                     "ESP_GMF_HLS"
#define HLS_MAX_LINE                512
#define HLS_MAX_SEG_COUNT           64
#define HLS_HTTP_TIMEOUT_MS         5000
#define HLS_READ_POLL_MS            50
#define HLS_PLAYLIST_RELOAD_MS      3000
#define HLS_MAX_RETRY               5

/* m3u8 line prefixes (case sensitive per RFC 8216) */
#define HLS_EXT_PREFIX              "#EXT"
#define HLS_SEG_PREFIX              "#EXTINF:"
#define HLS_MEDIA_SEQ_PREFIX        "#EXT-X-MEDIA-SEQUENCE:"
#define HLS_VARIANT_PREFIX          "#EXT-X-STREAM-INF:"
#define HLS_TARGET_DURATION_PREFIX  "#EXT-X-TARGETDURATION:"

typedef struct {
    char   *uri;             /*!< Absolute segment URL */
    uint64_t seq;            /*!< Media sequence number */
} hls_seg_t;

typedef struct hls_io {
    esp_gmf_io_t base;       /*!< Base IO object */
    bool is_open;
    hls_io_cfg_t cfg;        /*!< Saved configuration */
    char *playlist_uri;      /*!< Media playlist URL */
    char *base_url;          /*!< Directory part of playlist URI for relative joins */
    hls_seg_t segs[HLS_MAX_SEG_COUNT];
    int seg_count;
    int cur_seg;             /*!< Index of the segment to download next */
    uint64_t media_sequence; /*!< EXT-X-MEDIA-SEQUENCE of the current playlist */
    uint32_t target_duration;/*!< EXT-X-TARGETDURATION in seconds */
    uint64_t next_seq;       /*!< Media sequence to download next (live track) */
    int retry_count;         /*!< Consecutive failures */

    char *seg_buf;           /*!< Current segment buffer */
    size_t seg_len;          /*!< Current segment buffer length */
    size_t seg_off;          /*!< Current segment buffer read offset */

    esp_http_client_handle_t active_client;  /*!< In-flight HTTP client (for close abort) */
    SemaphoreHandle_t lock;  /*!< Guards playlist/segment state */
} hls_io_t;

static const char *TAG = HLS_TAG;

/* ---------------------------------------------------------------- helpers */

static char *hls_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/* Join a possibly-relative segment path against the playlist base URL. */
static char *hls_resolve_url(const char *base_url, const char *segment)
{
    if (strncasecmp(segment, "http://", 7) == 0 || strncasecmp(segment, "https://", 8) == 0) {
        return strdup(segment);
    }
    if (segment[0] == '/' && segment[1] == '/') {
        const char *scheme_end = strstr(base_url, "://");
        if (!scheme_end) {
            return NULL;
        }
        size_t scheme_len = (size_t)(scheme_end - base_url) + 1;
        size_t seg_len = strlen(segment);
        char *url = malloc(scheme_len + seg_len + 1);
        if (!url) {
            return NULL;
        }
        memcpy(url, base_url, scheme_len);
        memcpy(url + scheme_len, segment, seg_len);
        url[scheme_len + seg_len] = '\0';
        return url;
    }
    const char *slash = strrchr(base_url, '/');
    if (!slash) {
        return NULL;
    }
    size_t dir_len = (size_t)(slash - base_url) + 1;
    size_t seg_len = strlen(segment);
    char *url = malloc(dir_len + seg_len + 1);
    if (!url) {
        return NULL;
    }
    memcpy(url, base_url, dir_len);
    memcpy(url + dir_len, segment, seg_len);
    url[dir_len + seg_len] = '\0';
    return url;
}

static void hls_compute_base_url(hls_io_t *hls)
{
    free(hls->base_url);
    hls->base_url = NULL;
    const char *slash = strrchr(hls->playlist_uri, '/');
    if (!slash) {
        return;
    }
    size_t len = (size_t)(slash - hls->playlist_uri) + 1;
    hls->base_url = malloc(len + 1);
    if (hls->base_url) {
        memcpy(hls->base_url, hls->playlist_uri, len);
        hls->base_url[len] = '\0';
    }
}

/* Download a URL into a heap buffer. Uses a one-shot HTTP client. */
static esp_err_t hls_download(hls_io_t *hls, const char *url, char **out_buf, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HLS_HTTP_TIMEOUT_MS,
        .buffer_size = HLS_IO_BUFFER_SIZE,
        .buffer_size_tx = 1024,
    };
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    cfg.crt_bundle_attach = hls->cfg.crt_bundle_attach;
#else
    (void)hls;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    hls->active_client = client;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        hls->active_client = NULL;
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s -> HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        hls->active_client = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t cap = (content_length > 0) ? (size_t)content_length + 1 : (HLS_IO_RINGBUFFER_SIZE + 1);
    char *buf = malloc(cap);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        hls->active_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    int read_len;
    while (total < cap - 1 && hls->is_open && !hls->base._is_abort) {
        /* Poll with a short read timeout so a stop/abort request (which only
         * clears `is_open`, it cannot unblock esp_http_client_read from another
         * task on IDF < 5.5.3) is noticed promptly instead of stalling the whole
         * stop for HLS_HTTP_TIMEOUT_MS. */
        esp_http_client_set_timeout_ms(client, HLS_READ_POLL_MS);
        read_len = esp_http_client_read(client, buf + total, (int)(cap - 1 - total));
        if (read_len > 0) {
            total += (size_t)read_len;
        } else if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        } else {
            break;
        }
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    hls->active_client = NULL;

    if (!hls->is_open || hls->base._is_abort) {
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

static void hls_free_playlist(hls_io_t *hls)
{
    for (int i = 0; i < hls->seg_count; i++) {
        free(hls->segs[i].uri);
        hls->segs[i].uri = NULL;
    }
    hls->seg_count = 0;
    hls->cur_seg = 0;
}

/* Parse a media playlist into the segment list (with media sequence). */
static esp_err_t hls_parse_media_playlist(hls_io_t *hls, const char *body, size_t len)
{
    hls_free_playlist(hls);
    hls->media_sequence = 0;

    const char *p = body;
    const char *end = body + len;
    uint64_t index = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[HLS_MAX_LINE];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        p = nl ? nl + 1 : end;

        char *trimmed = hls_trim(line);
        if (strncmp(trimmed, HLS_MEDIA_SEQ_PREFIX, sizeof(HLS_MEDIA_SEQ_PREFIX) - 1) == 0) {
            hls->media_sequence = strtoull(trimmed + sizeof(HLS_MEDIA_SEQ_PREFIX) - 1, NULL, 10);
        } else if (strncmp(trimmed, HLS_TARGET_DURATION_PREFIX, sizeof(HLS_TARGET_DURATION_PREFIX) - 1) == 0) {
            hls->target_duration = (uint32_t)strtoul(trimmed + sizeof(HLS_TARGET_DURATION_PREFIX) - 1, NULL, 10);
        } else if (trimmed[0] != '\0' && trimmed[0] != '#') {
            /* plain URL line = segment */
            if (hls->seg_count < HLS_MAX_SEG_COUNT) {
                char *url = hls_resolve_url(hls->base_url ? hls->base_url : hls->playlist_uri, trimmed);
                if (url) {
                    hls->segs[hls->seg_count].uri = url;
                    hls->segs[hls->seg_count].seq = hls->media_sequence + index;
                    hls->seg_count++;
                }
            } else {
                ESP_LOGW(TAG, "segment list full (%d), ignoring extras", HLS_MAX_SEG_COUNT);
            }
            index++;
        }
    }

    ESP_LOGI(TAG, "Parsed %d segments (media sequence %llu, target duration %us)",
             hls->seg_count, (unsigned long long)hls->media_sequence, hls->target_duration);
    if (hls->seg_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Fetch and parse the media playlist for the given URL. */
static esp_err_t hls_load_playlist(hls_io_t *hls, const char *playlist_uri)
{
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = hls_download(hls, playlist_uri, &body, &len);
    if (err != ESP_OK) {
        return err;
    }

    /* Detect master playlist (contains #EXT-X-STREAM-INF) */
    if (strstr(body, "#EXT-X-STREAM-INF") != NULL) {
        /* pick first variant URI, then parse the media playlist */
        char variant_url[HLS_MAX_LINE] = {0};
        const char *vp = body;
        const char *vend = body + len;
        while (vp < vend) {
            const char *nl = memchr(vp, '\n', (size_t)(vend - vp));
            if (!nl) {
                break;
            }
            size_t line_len = (size_t)(nl - vp);
            char line[HLS_MAX_LINE];
            if (line_len >= sizeof(line)) {
                line_len = sizeof(line) - 1;
            }
            memcpy(line, vp, line_len);
            line[line_len] = '\0';
            vp = nl + 1;

            char *trimmed = hls_trim(line);
            if (trimmed[0] != '\0' && trimmed[0] != '#') {
                strncpy(variant_url, trimmed, sizeof(variant_url) - 1);
                variant_url[sizeof(variant_url) - 1] = '\0';
                break;
            }
        }
        free(body);

        if (variant_url[0] == '\0') {
            ESP_LOGE(TAG, "master playlist has no usable variant");
            return ESP_ERR_NOT_FOUND;
        }
        char *resolved = hls_resolve_url(hls->base_url ? hls->base_url : playlist_uri, variant_url);
        if (!resolved) {
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Master playlist -> media playlist %s", resolved);
        /* treat the variant media playlist as the new playlist uri */
        free(hls->playlist_uri);
        hls->playlist_uri = resolved;
        hls_compute_base_url(hls);
        err = hls_load_playlist(hls, resolved);
        return err;
    }

    err = hls_parse_media_playlist(hls, body, len);
    free(body);
    return err;
}

/* ------------------------------------------------------------ IO vtable */

static esp_gmf_err_t _hls_get_score(esp_gmf_io_handle_t handle, const char *url, int *score)
{
    (void)handle;
    *score = ESP_GMF_IO_SCORE_NONE;
    if (!url) {
        return ESP_GMF_ERR_OK;
    }
    size_t len = strlen(url);
    if (len >= 5 && strncasecmp(url + len - 5, ".m3u8", 5) == 0) {
        *score = ESP_GMF_IO_SCORE_PERFECT;
    }
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _hls_open(esp_gmf_io_handle_t self)
{
    hls_io_t *hls = (hls_io_t *)self;
    if (hls->is_open) {
        return ESP_GMF_ERR_OK;
    }

    esp_gmf_info_file_t info = {0};
    esp_gmf_io_get_info(self, &info);
    if (!info.uri) {
        ESP_LOGE(TAG, "open: no uri");
        return ESP_GMF_ERR_FAIL;
    }

    xSemaphoreTake(hls->lock, portMAX_DELAY);
    free(hls->playlist_uri);
    hls->playlist_uri = strdup(info.uri);
    hls_compute_base_url(hls);
    hls->seg_buf = NULL;
    hls->seg_len = 0;
    hls->seg_off = 0;
    hls->next_seq = 0;
    hls->retry_count = 0;
    /* mark open before loading the playlist so a blocked download (which checks
     * is_open/_is_abort every 200ms) does not abort the very first fetch */
    hls->is_open = true;
    xSemaphoreGive(hls->lock);

    esp_err_t err = hls_load_playlist(hls, hls->playlist_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to load playlist: %s", esp_err_to_name(err));
        return ESP_GMF_ERR_FAIL;
    }

    xSemaphoreTake(hls->lock, portMAX_DELAY);
    hls->cur_seg = 0;
    hls->next_seq = hls->segs[0].seq;
    xSemaphoreGive(hls->lock);

    ESP_LOGI(TAG, "HLS opened: %s", hls->playlist_uri);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _hls_prev_close(esp_gmf_io_handle_t self)
{
    hls_io_t *hls = (hls_io_t *)self;
    /* Mark the stream closed so a blocked acquire_read exits on its next turn,
     * letting esp_gmf_io_close (which waits for the IO task) complete quickly. */
    hls->is_open = false;
    /* unblock any in-flight HTTP read so esp_gmf_io_close can proceed */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 3)
    if (hls->active_client) {
        int fd = esp_http_client_get_socket(hls->active_client);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
    }
#endif
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _hls_close(esp_gmf_io_handle_t self)
{
    hls_io_t *hls = (hls_io_t *)self;
    ESP_LOGD(TAG, "hls close %p", hls);
    hls->is_open = false;

    xSemaphoreTake(hls->lock, portMAX_DELAY);
    free(hls->seg_buf);
    hls->seg_buf = NULL;
    hls->seg_len = 0;
    hls->seg_off = 0;
    hls_free_playlist(hls);
    free(hls->playlist_uri);
    hls->playlist_uri = NULL;
    free(hls->base_url);
    hls->base_url = NULL;
    xSemaphoreGive(hls->lock);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _hls_seek(esp_gmf_io_handle_t self, uint64_t pos)
{
    (void)self;
    (void)pos;
    return ESP_GMF_ERR_NOT_SUPPORT;
}

static esp_gmf_err_t _hls_reload(esp_gmf_io_handle_t self, const char *uri)
{
    _hls_close(self);
    esp_gmf_io_set_uri(self, uri);
    return _hls_open(self);
}

/* Refetch the live playlist and advance to the next unseen segment. */
static esp_err_t hls_refresh_playlist(hls_io_t *hls)
{
    esp_err_t err = hls_load_playlist(hls, hls->playlist_uri);
    if (err != ESP_OK) {
        return err;
    }
    /* find the first segment with seq >= next_seq */
    int idx = -1;
    for (int i = 0; i < hls->seg_count; i++) {
        if (hls->segs[i].seq >= hls->next_seq) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* window has advanced past us; skip to the newest segment */
        idx = 0;
        hls->next_seq = hls->segs[0].seq;
        ESP_LOGW(TAG, "live window advanced, skipping to seq %llu",
                 (unsigned long long)hls->next_seq);
    }
    hls->cur_seg = idx;
    return ESP_OK;
}

/* Ensure a full segment is buffered and ready at `seg_off`. */
static esp_err_t hls_ensure_segment(hls_io_t *hls)
{
    if (hls->seg_buf && hls->seg_off < hls->seg_len) {
        return ESP_OK;
    }
    free(hls->seg_buf);
    hls->seg_buf = NULL;
    hls->seg_len = 0;
    hls->seg_off = 0;

    xSemaphoreTake(hls->lock, portMAX_DELAY);
    int idx = hls->cur_seg;
    if (idx >= hls->seg_count) {
        /* window exhausted: refresh playlist for live, EOF for VOD */
        xSemaphoreGive(hls->lock);
        esp_err_t err = hls_refresh_playlist(hls);
        if (err != ESP_OK) {
            return err;
        }
        idx = hls->cur_seg;
    }
    hls_seg_t seg = hls->segs[idx];
    hls->cur_seg = idx + 1;
    xSemaphoreGive(hls->lock);

    char *data = NULL;
    size_t data_len = 0;
    esp_err_t err = hls_download(hls, seg.uri, &data, &data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "segment %d (seq %llu) download failed: %s", idx,
                 (unsigned long long)seg.seq, esp_err_to_name(err));
        if (hls->is_open && !hls->base._is_abort && hls->retry_count++ < HLS_MAX_RETRY) {
            /* rewind this segment for retry (not on abort) */
            xSemaphoreTake(hls->lock, portMAX_DELAY);
            hls->cur_seg = idx;
            xSemaphoreGive(hls->lock);
        }
        return err;
    }

    xSemaphoreTake(hls->lock, portMAX_DELAY);
    hls->seg_buf = data;
    hls->seg_len = data_len;
    hls->seg_off = 0;
    hls->next_seq = seg.seq + 1;
    hls->retry_count = 0;
    xSemaphoreGive(hls->lock);
    ESP_LOGD(TAG, "buffered segment %d, %d bytes, seq %llu", idx, (int)data_len,
             (unsigned long long)seg.seq);
    return ESP_OK;
}

static esp_gmf_err_io_t _hls_acquire_read(esp_gmf_io_handle_t self, void *payload, uint32_t wanted_size, int block_ticks)
{
    hls_io_t *hls = (hls_io_t *)self;
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;

    if (!hls->is_open || hls->base._is_abort) {
        return ESP_GMF_IO_ABORT;
    }

    /* serve from the current segment buffer */
    if (hls->seg_buf && hls->seg_off < hls->seg_len) {
        uint32_t n = wanted_size;
        size_t remain = hls->seg_len - hls->seg_off;
        if (n > remain) {
            n = (uint32_t)remain;
        }
        memcpy(pload->buf, hls->seg_buf + hls->seg_off, n);
        hls->seg_off += n;
        pload->valid_size = n;
        pload->is_done = false;
        return ESP_GMF_IO_OK;
    }

    /* fetch the next segment (blocking; data bus decouples the decoder) */
    esp_err_t err = hls_ensure_segment(hls);
    if (err != ESP_OK) {
        if (!hls->is_open || hls->base._is_abort) {
            return ESP_GMF_IO_ABORT;
        }
        if (hls->retry_count >= HLS_MAX_RETRY) {
            ESP_LOGE(TAG, "HLS stream failed after %d retries", HLS_MAX_RETRY);
            return ESP_GMF_IO_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        pload->valid_size = 0;
        pload->is_done = false;
        return ESP_GMF_IO_OK;
    }

    uint32_t n = wanted_size;
    size_t remain = hls->seg_len - hls->seg_off;
    if (n > remain) {
        n = (uint32_t)remain;
    }
    memcpy(pload->buf, hls->seg_buf + hls->seg_off, n);
    hls->seg_off += n;
    pload->valid_size = n;
    pload->is_done = false;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _hls_release_read(esp_gmf_io_handle_t self, void *payload, int block_ticks)
{
    (void)self;
    (void)payload;
    (void)block_ticks;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_t _hls_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return esp_gmf_io_hls_init(cfg, io);
}

static esp_gmf_err_t _hls_destroy(esp_gmf_io_handle_t self)
{
    hls_io_t *hls = (hls_io_t *)self;
    _hls_close(self);
    void *cfg = OBJ_GET_CFG(self);
    if (cfg) {
        esp_gmf_oal_free(cfg);
    }
    esp_gmf_io_deinit(self);
    if (hls->lock) {
        vSemaphoreDelete(hls->lock);
        hls->lock = NULL;
    }
    esp_gmf_oal_free(hls);
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t esp_gmf_io_hls_init(hls_io_cfg_t *config, esp_gmf_io_handle_t *io)
{
    ESP_GMF_NULL_CHECK(TAG, config, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, io, return ESP_GMF_ERR_INVALID_ARG);
    *io = NULL;
    esp_gmf_err_t ret = ESP_GMF_ERR_OK;

    hls_io_t *hls = esp_gmf_oal_calloc(1, sizeof(hls_io_t));
    ESP_GMF_MEM_VERIFY(TAG, hls, return ESP_GMF_ERR_MEMORY_LACK, "hls stream", sizeof(hls_io_t));

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)hls;
    obj->new_obj = _hls_new;
    obj->del_obj = _hls_destroy;
    hls_io_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(*config));
    ESP_GMF_MEM_VERIFY(TAG, cfg, {ret = ESP_GMF_ERR_MEMORY_LACK; goto _hls_init_fail;},
                       "hls stream configuration", sizeof(*config));
    memcpy(cfg, config, sizeof(*config));
    esp_gmf_obj_set_config(obj, cfg, sizeof(*config));
    hls->cfg = *config;
    ret = esp_gmf_obj_set_tag(obj, "io_hls");
    ESP_GMF_RET_ON_NOT_OK(TAG, ret, goto _hls_init_fail, "Failed to set obj tag");

    hls->base.dir = (esp_gmf_io_dir_t)config->dir;
    hls->base.type = ESP_GMF_IO_TYPE_BLOCK;
    hls->base.get_score = _hls_get_score;
    hls->base.open = _hls_open;
    hls->base.seek = _hls_seek;
    hls->base.prev_close = _hls_prev_close;
    hls->base.close = _hls_close;
    hls->base.reload = _hls_reload;
    hls->base.acquire_read = _hls_acquire_read;
    hls->base.release_read = _hls_release_read;

    esp_gmf_io_cfg_t io_cfg = {
        .thread = {
            .stack = config->io_cfg.thread.stack,
            .prio = config->io_cfg.thread.prio,
            .core = config->io_cfg.thread.core,
            .stack_in_ext = config->io_cfg.thread.stack_in_ext,
        },
        .buffer_cfg = {
            .io_size = config->io_cfg.buffer_cfg.io_size,
            .buffer_size = config->io_cfg.buffer_cfg.buffer_size,
        },
        .enable_speed_monitor = config->io_cfg.enable_speed_monitor,
    };

    hls->lock = xSemaphoreCreateMutex();
    if (!hls->lock) {
        ret = ESP_GMF_ERR_MEMORY_LACK;
        goto _hls_init_fail;
    }

    ret = esp_gmf_io_init(&hls->base, &io_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        goto _hls_init_fail;
    }

    *io = obj;
    ESP_LOGD(TAG, "Initialization, %s-%p", OBJ_GET_TAG(hls), hls);
    return ESP_GMF_ERR_OK;
_hls_init_fail:
    esp_gmf_obj_delete(obj);
    return ret;
}
