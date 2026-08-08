# esp-gmf managed_components patches

`application/edge_agent/managed_components/` is gitignored, so fixes applied inside
it are NOT tracked and are lost on a fresh clone. These `.patch` files preserve
them so they can be re-applied after `idf.py build` re-downloads the components.

## Version

- Component: `espressif/gmf_core` **v0.8.4** (commit `5ef03925`, repository
  `git://github.com/espressif/esp-gmf.git`, path `./gmf_core`)
- Component: `espressif/gmf_io` **v0.8.1** (`application/edge_agent/managed_components/espressif__gmf_io`)
- Target project: `application/edge_agent`
- Verified against ESP-IDF **v5.5.1**

## Apply

After a fresh clone (or `idf.py fullclean`), from `application/edge_agent` run:

```bash
cd application/edge_agent/managed_components
patch -p1 --forward < ../../../patches/esp-gmf/esp_gmf_io.c.patch
patch -p1 --forward < ../../../patches/esp-gmf/esp_gmf_task.c.patch
```

`audio_hls_io.c` / `audio_hls_io.h` in `espressif__gmf_io` are **full-file backups**
(not unified diffs): the pristine sources are not present in the Component Manager
cache, so copy them back wholesale:

```bash
cp ../../../patches/esp-gmf/audio_hls_io.c espressif__gmf_io/src/audio_hls_io.c
cp ../../../patches/esp-gmf/audio_hls_io.h espressif__gmf_io/include/audio_hls_io.h
```

Sanity check: after applying, the files should byte-match the working tree
(until the next gmf_core bump):

```bash
diff -q espressif__gmf_core/src/esp_gmf_io.c \
      <(patch -R -p1 -d espressif__gmf_core/src < ../../../patches/esp-gmf/esp_gmf_io.c.patch -o /dev/null)
```

## What each patch fixes

### esp_gmf_io.c

radio_player 停止/切台死锁 + HLS 停止阻塞的三处修复：

1. `io_process_read()`: read 返回 `ESP_GMF_IO_ABORT` 时映射为
   `ESP_GMF_JOB_ERR_ABORT`（原来被吞成 `FAIL`），让 abort 沿 terminate 流退出。
2. `esp_gmf_io_process()`: ABORT 透传门槛加 `!io->_is_abort`——abort 是终止信号，
   不应被 HOLD 逻辑吞掉。
3. `esp_gmf_io_abort()`: 不设置 `_is_hold`（避免 worker 卡在无界的 HOLD_DONE 等待），
   并调用 `io->prev_close`（仅 HLS 实现）中断阻塞在
   `esp_http_client_open`/`fetch_headers`（TLS 握手，不轮询 `_is_abort`）的 worker，
   使 stop/切台在有限时间内完成。

### esp_gmf_task.c

`esp_gmf_task_stop()`: 停止超时后不再无限等待 STOP bit
（原 `0xFFFFFFFF`，worker 卡在不可中断 IO 时会永久冻结调用线程/LVGL UI），
改为再等一个 `api_sync_time` 窗口，仍不到则返回 `ESP_GMF_ERR_TIMEOUT`。

### audio_hls_io.c / audio_hls_io.h（整文件备份）

`espressif__gmf_io` 的 HLS IO 存在三处本地修改（原始文件不在 Component
Manager 缓存，故整文件备份）：

1. **`_hls_new` 透传创建**（RTHK HLS 收音机）：`esp_gmf_pool_new_io` 调
   `esp_gmf_obj_dupl` 要求 IO 有 `new_obj`；io_http 有，io_hls 原是 NULL →
   `esp_gmf_obj_dupl is no new function`。新增 `_hls_new`（透传
   `esp_gmf_io_hls_init`）并赋值 `obj->new_obj = _hls_new`。
2. **HTTPS 证书**（RTHK HLS）：HLS cfg 默认 `crt_bundle_attach=NULL` →
   `esp-tls-mbedtls: No server verification option set` → 建连失败。顶层
   `#include "esp_crt_bundle.h"` + `hls_cfg.crt_bundle_attach =
   esp_crt_bundle_attach;`。
3. **协议相对分片 URL 支持**（深圳蜻蜓 FM）：蜻蜓 live 流的 segment 是
   `//ls-hw-ot.qtfm.cn/...`（协议相对）。`hls_resolve_url()` 原来只处理
   绝对 URL 和目录相对路径，`//host/...` 被错误拼成
   `https://ls.qingting.fm/live//ls-hw-ot.qtfm.cn/...` → HTTP 解析失败
   （`Error parse url` / `ESP_ERR_INVALID_RESPONSE`）。修复：`segment` 以
   `//` 开头时，取 base_url 的 scheme 段（`scheme_len = strstr(base_url,"://")
   - base_url + 1`，即 `"https:"`）拼上 `segment` 自身自带的 `//` 得到
   `https://ls-hw-ot.qtfm.cn/...`。

验证：设备实测深圳先锋898/飞扬971/快乐1062/私家车94.2/星光FM99.1、
CNR 中国之声/经典音乐广播、RTHK 转播香港之声全部 `RUNNING`（2026-08-08）。

## Regenerating

Base copies live in the Espressif component manager cache, e.g.:

```
~/.cache/Espressif/ComponentManager/service_*/espressif__gmf_core_0.8.4_6e11017f/src/
```

Regenerate with:

```bash
diff -u <cache>/src/esp_gmf_io.c <managed>/espressif__gmf_core/src/esp_gmf_io.c \
  > patches/esp-gmf/esp_gmf_io.c.patch
```
