# esp-gmf managed_components patches

`application/edge_agent/managed_components/` is gitignored, so fixes applied inside
it are NOT tracked and are lost on a fresh clone. These `.patch` files preserve
them so they can be re-applied after `idf.py build` re-downloads the components.

## Version

- Component: `espressif/gmf_core` **v0.8.4** (commit `5ef03925`, repository
  `git://github.com/espressif/esp-gmf.git`, path `./gmf_core`)
- Target project: `application/edge_agent`
- Verified against ESP-IDF **v5.5.1**

## Apply

After a fresh clone (or `idf.py fullclean`), from `application/edge_agent` run:

```bash
cd application/edge_agent/managed_components
patch -p1 --forward < ../../../patches/esp-gmf/esp_gmf_io.c.patch
patch -p1 --forward < ../../../patches/esp-gmf/esp_gmf_task.c.patch
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
