/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_web_search.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_web_search";

#define CAP_WEB_SEARCH_BUF_SIZE     (16 * 1024)
#define CAP_WEB_SEARCH_RESULT_COUNT 5

typedef enum {
    CAP_WEB_SEARCH_PROVIDER_NONE = 0,
    CAP_WEB_SEARCH_PROVIDER_BRAVE,
    CAP_WEB_SEARCH_PROVIDER_TAVILY,
    CAP_WEB_SEARCH_PROVIDER_BING,
} cap_web_search_provider_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_web_search_buf_t;

typedef struct {
    char brave_key[128];
    char tavily_key[128];
    cap_web_search_provider_t provider;
} cap_web_search_state_t;

static EXT_RAM_BSS_ATTR cap_web_search_state_t s_search = {0};

static void cap_web_search_refresh_provider(void)
{
    if (s_search.tavily_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_TAVILY;
    } else if (s_search.brave_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BRAVE;
    } else {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BING;
    }
}

static esp_err_t cap_web_search_http_event_handler(esp_http_client_event_t *event)
{
    cap_web_search_buf_t *buf = NULL;
    size_t append_len;

    if (!event) {
        return ESP_OK;
    }

    buf = (cap_web_search_buf_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buf || !buf->data || event->data_len <= 0) {
        return ESP_OK;
    }

    append_len = (size_t)event->data_len;
    if (buf->len + append_len + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        char *new_data = NULL;

        if (new_cap < buf->len + append_len + 1) {
            new_cap = buf->len + append_len + 1;
        }
        new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, event->data, append_len);
    buf->len += append_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static size_t cap_web_search_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (*src && pos < dst_size - 1) {
        unsigned char c = (unsigned char) * src;

        if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            if (pos + 3 >= dst_size) {
                break;
            }
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
        src++;
    }

    dst[pos] = '\0';
    return pos;
}

static void cap_web_search_format_brave_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = NULL;
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    web = cJSON_GetObjectItem(root, "web");
    results = web ? cJSON_GetObjectItem(web, "results") : NULL;
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *description = NULL;
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        description = cJSON_GetObjectItem(item, "description");
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           cJSON_IsString(description) ? description->valuestring : "");
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static void cap_web_search_format_tavily_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *content = NULL;
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        content = cJSON_GetObjectItem(item, "content");
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           cJSON_IsString(content) ? content->valuestring : "");
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static char *cap_web_search_build_tavily_payload(const char *query)
{
    cJSON *root = NULL;
    char *payload = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "query", query);
    cJSON_AddNumberToObject(root, "max_results", CAP_WEB_SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(root, "include_answer", false);
    cJSON_AddStringToObject(root, "search_depth", "basic");
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static esp_err_t cap_web_search_brave_direct(const char *url, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search.brave_key);
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Brave search returned %d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_bing_direct(const char *url, cap_web_search_buf_t *buf)
{
    const int max_attempts = 3;
    int attempt;

    for (attempt = 0; attempt < max_attempts; attempt++) {
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = cap_web_search_http_event_handler,
            .user_data = buf,
            .timeout_ms = 15000,
            .buffer_size = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = NULL;
        esp_err_t err;
        int status;

        client = esp_http_client_init(&config);
        if (!client) {
            return ESP_FAIL;
        }
        buf->len = 0;
        if (buf->data) {
            buf->data[0] = '\0';
        }

        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err == ESP_OK && status == 200) {
            return ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Bing search transient failure (attempt %d/%d): %s",
                     attempt + 1, max_attempts, esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "Bing search returned %d", status);
            return ESP_FAIL;
        }
        if (attempt < max_attempts - 1) {
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
        }
    }

    return ESP_ERR_HTTP_CONNECT;
}

static char *cap_web_search_html_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    size_t i = 0;

    if (!src || !dst || dst_size == 0) {
        return NULL;
    }

    while (src[i] && out < dst_size - 1) {
        if (src[i] == '&' && src[i + 1] == 'a' && src[i + 2] == 'm' && src[i + 3] == 'p' &&
                src[i + 4] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = '&';
            i += 5;
        } else if (src[i] == '&' && src[i + 1] == 'l' && src[i + 2] == 't' && src[i + 3] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = '<';
            i += 4;
        } else if (src[i] == '&' && src[i + 1] == 'g' && src[i + 2] == 't' && src[i + 3] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = '>';
            i += 4;
        } else if (src[i] == '&' && src[i + 1] == 'q' && src[i + 2] == 'u' && src[i + 3] == 'o' &&
                src[i + 4] == 't' && src[i + 5] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = '"';
            i += 6;
        } else if (src[i] == '&' && src[i + 1] == '#' && src[i + 2] == '3' && src[i + 3] == '9' &&
                src[i + 4] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = '\'';
            i += 5;
        } else if (src[i] == '&' && src[i + 1] == 'n' && src[i + 2] == 'b' && src[i + 3] == 's' &&
                src[i + 4] == 'p' && src[i + 5] == ';') {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = ' ';
            i += 6;
        } else if (src[i] == '&' && src[i + 1] == '#') {
            const char *digits = src + i + 2;
            int value = 0;
            bool valid = true;

            while (*digits >= '0' && *digits <= '9' && (size_t)(digits - src) < i + 8) {
                value = value * 10 + (*digits - '0');
                digits++;
            }
            if (*digits == ';' && value > 0 && value <= 0x10FFFF) {
                if (out + 1 >= dst_size) {
                    break;
                }
                if (value < 0x80) {
                    dst[out++] = (char)value;
                } else {
                    dst[out++] = ' ';
                }
                i = (size_t)(digits - src) + 1;
            } else {
                valid = false;
            }
            if (!valid) {
                dst[out++] = src[i];
                i++;
            }
        } else {
            dst[out++] = src[i];
            i++;
        }
    }
    dst[out] = '\0';
    return dst;
}

static char *cap_web_search_strip_tags(char *s)
{
    char *read = s;
    char *write = s;

    if (!s) {
        return NULL;
    }

    while (*read) {
        if (*read == '<') {
            char *end = strchr(read, '>');
            if (end) {
                read = end + 1;
                continue;
            }
        }
        *write++ = *read++;
    }
    *write = '\0';
    return s;
}

static int cap_web_search_base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static size_t cap_web_search_base64_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    size_t i = 0;
    size_t src_len = strlen(src);

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (i + 3 < src_len && out + 3 < dst_size) {
        int v0 = cap_web_search_base64_value(src[i]);
        int v1 = cap_web_search_base64_value(src[i + 1]);
        int v2 = cap_web_search_base64_value(src[i + 2]);
        int v3 = cap_web_search_base64_value(src[i + 3]);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            break;
        }
        dst[out++] = (char)((v0 << 2) | (v1 >> 4));
        dst[out++] = (char)(((v1 & 0x0F) << 4) | (v2 >> 2));
        dst[out++] = (char)(((v2 & 0x03) << 6) | v3);
        i += 4;
    }
    dst[out] = '\0';
    return out;
}

static size_t cap_web_search_extract_bing_url(const char *href, char *dst, size_t dst_size)
{
    const char *u = NULL;
    const char *end = NULL;
    char decoded[320];
    size_t len;

    if (!href || !dst || dst_size == 0) {
        return 0;
    }

    cap_web_search_html_decode(href, decoded, sizeof(decoded));
    u = strstr(decoded, "u=");
    if (!u) {
        len = strlen(decoded);
        if (len + 1 > dst_size) {
            len = dst_size - 1;
        }
        memcpy(dst, decoded, len);
        dst[len] = '\0';
        return len;
    }
    u += 2;
    end = strchr(u, '&');
    if (!end) {
        end = u + strlen(u);
    }
    len = (size_t)(end - u);
    if (len >= 2 && u[0] == 'a' && u[1] == '1') {
        u += 2;
        len -= 2;
    }
    if (len + 1 > sizeof(decoded)) {
        len = sizeof(decoded) - 1;
    }
    memcpy(decoded, u, len);
    decoded[len] = '\0';
    return cap_web_search_base64_decode(decoded, dst, dst_size);
}

static esp_err_t cap_web_search_parse_bing(const char *html,
                                           char *output,
                                           size_t output_size)
{
    const char *pos = html;
    size_t offset = 0;
    int index = 0;

    if (!html || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (index < CAP_WEB_SEARCH_RESULT_COUNT && offset < output_size - 1) {
        const char *algo = NULL;
        const char *algo_end = NULL;
        const char *h2 = NULL;
        const char *href = NULL;
        const char *href_end = NULL;
        const char *title_start = NULL;
        const char *title_end = NULL;
        const char *snip = NULL;
        const char *snip_end = NULL;
        char title[192];
        char url[320];
        char snippet[256];
        int written;

        algo = strstr(pos, "b_algo");
        if (!algo) {
            break;
        }
        algo_end = strstr(algo, "</li>");
        if (!algo_end) {
            break;
        }
        h2 = strstr(algo, "<h2");
        if (h2 && h2 < algo_end) {
            href = strstr(h2, "href=\"");
            if (href && href < algo_end) {
                href += 6;
                href_end = strchr(href, '"');
                if (href_end && href_end < algo_end) {
                    title_start = strstr(href_end, ">");
                    if (title_start && title_start < algo_end) {
                        title_start++;
                        title_end = strstr(title_start, "</a>");
                        if (title_end && title_end < algo_end) {
                            size_t title_len = (size_t)(title_end - title_start);

                            if (title_len > sizeof(title) - 1) {
                                title_len = sizeof(title) - 1;
                            }
                            memcpy(title, title_start, title_len);
                            title[title_len] = '\0';
                        } else {
                            title[0] = '\0';
                        }
                    } else {
                        title[0] = '\0';
                    }
                } else {
                    title[0] = '\0';
                }
            } else {
                title[0] = '\0';
            }
        } else {
            title[0] = '\0';
        }

        if (href && href_end) {
            size_t href_len = (size_t)(href_end - href);

            if (href_len > 320 - 1) {
                href_len = 320 - 1;
            }
            memcpy(url, href, href_len);
            url[href_len] = '\0';
        } else {
            url[0] = '\0';
        }

        snip = strstr(algo, "<p");
        if (snip && snip < algo_end) {
            snip = strchr(snip, '>');
            if (snip && snip < algo_end) {
                snip++;
                snip_end = strstr(snip, "</p>");
                if (snip_end && snip_end < algo_end) {
                    size_t snip_len = (size_t)(snip_end - snip);

                    if (snip_len > sizeof(snippet) - 1) {
                        snip_len = sizeof(snippet) - 1;
                    }
                    memcpy(snippet, snip, snip_len);
                    snippet[snip_len] = '\0';
                } else {
                    snippet[0] = '\0';
                }
            } else {
                snippet[0] = '\0';
            }
        } else {
            snippet[0] = '\0';
        }

        cap_web_search_strip_tags(title);
        cap_web_search_strip_tags(snippet);
        cap_web_search_html_decode(title, title, sizeof(title));
        cap_web_search_html_decode(snippet, snippet, sizeof(snippet));
        if (url[0]) {
            cap_web_search_extract_bing_url(url, url, sizeof(url));
        }

        if (!title[0]) {
            pos = algo_end + 5;
            continue;
        }

        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           title,
                           url,
                           snippet);
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return ESP_OK;
        }
        offset += (size_t)written;
        index++;
        pos = algo_end + 5;
    }

    if (index == 0) {
        snprintf(output, output_size, "No web results found.");
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_tavily_direct(const char *query, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    char auth[192];
    char *payload = NULL;
    esp_err_t err;
    int status;

    payload = cap_web_search_build_tavily_payload(query);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    snprintf(auth, sizeof(auth), "Bearer %s", s_search.tavily_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Tavily search returned %d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    cJSON *input = NULL;
    cJSON *query = NULL;
    cap_web_search_buf_t buf = {0};
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_web_search_refresh_provider();
    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_NONE) {
        snprintf(output, output_size, "Error: no search provider credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    query = cJSON_GetObjectItem(input, "query");
    if (!cJSON_IsString(query) || !query->valuestring || !query->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing query");
        return ESP_ERR_INVALID_ARG;
    }

    buf.data = calloc(1, CAP_WEB_SEARCH_BUF_SIZE);
    if (!buf.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    buf.cap = CAP_WEB_SEARCH_BUF_SIZE;

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        err = cap_web_search_tavily_direct(query->valuestring, &buf);
        if (err != ESP_OK) {
            char encoded_query[256];
            char url[512];

            ESP_LOGW(TAG, "Tavily search failed (%s), falling back to Bing", esp_err_to_name(err));
            cap_web_search_url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
            snprintf(url,
                     sizeof(url),
                     "https://www.bing.com/search?q=%s&count=%d",
                     encoded_query,
                     CAP_WEB_SEARCH_RESULT_COUNT);
            err = cap_web_search_bing_direct(url, &buf);
            if (err == ESP_OK) {
                s_search.provider = CAP_WEB_SEARCH_PROVIDER_BING;
            }
        }
    } else if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_BRAVE) {
        char encoded_query[256];
        char url[512];

        cap_web_search_url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
        snprintf(url,
                 sizeof(url),
                 "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
                 encoded_query,
                 CAP_WEB_SEARCH_RESULT_COUNT);
        err = cap_web_search_brave_direct(url, &buf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Brave search failed (%s), falling back to Bing", esp_err_to_name(err));
            snprintf(url,
                     sizeof(url),
                     "https://www.bing.com/search?q=%s&count=%d",
                     encoded_query,
                     CAP_WEB_SEARCH_RESULT_COUNT);
            err = cap_web_search_bing_direct(url, &buf);
            if (err == ESP_OK) {
                s_search.provider = CAP_WEB_SEARCH_PROVIDER_BING;
            }
        }
    } else {
        char encoded_query[256];
        char url[512];

        cap_web_search_url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
        snprintf(url,
                 sizeof(url),
                 "https://www.bing.com/search?q=%s&count=%d",
                 encoded_query,
                 CAP_WEB_SEARCH_RESULT_COUNT);
        err = cap_web_search_bing_direct(url, &buf);
    }

    cJSON_Delete(input);
    if (err != ESP_OK) {
        free(buf.data);
        snprintf(output, output_size, "Error: search request failed (%s)", esp_err_to_name(err));
        return err;
    }

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        root = cJSON_Parse(buf.data);
        free(buf.data);
        if (!root) {
            snprintf(output, output_size, "Error: failed to parse search results");
            return ESP_FAIL;
        }
        cap_web_search_format_tavily_results(root, output, output_size);
        cJSON_Delete(root);
    } else if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_BRAVE) {
        root = cJSON_Parse(buf.data);
        free(buf.data);
        if (!root) {
            snprintf(output, output_size, "Error: failed to parse search results");
            return ESP_FAIL;
        }
        cap_web_search_format_brave_results(root, output, output_size);
        cJSON_Delete(root);
    } else {
        err = cap_web_search_parse_bing(buf.data, output, output_size);
        free(buf.data);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_web_search_descriptors[] = {
    {
        .id = "web_search",
        .name = "web_search",
        .family = "system",
        .description = "Search the web with the configured provider and return concise formatted results.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
        .execute = cap_web_search_execute,
    },
};

static const claw_cap_group_t s_web_search_group = {
    .group_id = "cap_web_search",
    .descriptors = s_web_search_descriptors,
    .descriptor_count = sizeof(s_web_search_descriptors) / sizeof(s_web_search_descriptors[0]),
};

esp_err_t cap_web_search_register_group(void)
{
    if (claw_cap_group_exists(s_web_search_group.group_id)) {
        return ESP_OK;
    }

    cap_web_search_refresh_provider();
    return claw_cap_register_group(&s_web_search_group);
}

esp_err_t cap_web_search_set_brave_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.brave_key, api_key, sizeof(s_search.brave_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}

esp_err_t cap_web_search_set_tavily_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.tavily_key, api_key, sizeof(s_search.tavily_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}
