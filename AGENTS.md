# Agents.md

This file provides guidance to agents when working with code in this repository.

## Project Overview

ESP-Claw is an ESP-IDF firmware project for running an AI agent framework on Espressif IoT devices. The main application is `application/edge_agent/`; reusable firmware components live under `components/`. The repo also contains board definitions, build-time FATFS content, documentation, and the embedded device settings UI.

## Development Commands

Export ESP-IDF before firmware work:

```bash
. $IDF_PATH/export.sh
```

Generate board manager files and build from the app directory:

```bash
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1
idf.py build
idf.py flash monitor
```

Docs site:

```bash
cd docs
pnpm install
pnpm build
pnpm dev
```

Embedded settings UI:

```bash
cd application/edge_agent/components/http_server/frontend_source
pnpm build
pnpm typecheck
```

## High-Level Architecture

### Boot and Runtime Flow

The main entry point is `application/edge_agent/main/main.c`. 

### Core Data Flow

1. IM channels, scheduler jobs, Lua scripts, startup hooks, or CLI commands publish events or submit requests.
2. `claw_event_router` matches events against the DATA root's `router_rules/router_rules.json` and can call capabilities, run scripts, run the agent, send messages, emit events, or drop events.
3. `claw_core` builds context from memory, session history, skills, and other providers; calls the configured LLM backend; executes capability tool calls; persists context; and returns responses.
4. Outbound messages are routed back through registered IM bindings or local/web channels.

## Key Subsystems

- **Application shell** (`application/edge_agent/main/main.c`, `components/common/app_claw/`): boot flow, storage paths, capability registration, Lua module registration, CLI, and agent startup.
- **Agent core** (`components/claw_modules/claw_core/`): request queue, context building, LLM backend runtime, tool-call loop, media inference, interrupts, context persistence, and response delivery.
- **Event router** (`components/claw_modules/claw_event_router/`): declarative event routing and actions backed by router rules in FATFS.
- **Capability registry** (`components/claw_modules/claw_cap/`): common registration and dispatch layer for model-callable capabilities.
- **Capabilities** (`components/claw_capabilities/`): concrete agent capabilities such as Lua execution, files, IM platforms, MCP, skill management, router management, scheduler, session management, time, HTTP requests, web search, system, and LLM inspection.
- **Memory** (`components/claw_modules/claw_memory/`): session history, profile/long-term memory providers, memory persistence, request gating, and stage notes.
- **Skills** (`components/claw_modules/claw_skill/`, component `skills/` directories): user-facing skill documents and activation state.
- **Lua modules** (`components/lua_modules/`): Lua drivers and higher-level modules for hardware, media, HTTP server, storage, threading, JSON, board manager, and capability calls.
- **Board manager** (`application/edge_agent/boards/`): board metadata, peripheral YAML, board setup code, board defaults, optional local components, and optional board FATFS overlays.
- **FATFS images** (`application/edge_agent/fatfs_image/`): build-time source trees for the read-only SYSTEM image and writable DATA seed image.
- **HTTP config service** (`application/edge_agent/components/http_server/`): local device configuration server and embedded frontend.

### Runtime Path Rules

The firmware uses two logical filesystem roots, configured at boot through `claw_paths`:

- `CLAW_PATH_SYSTEM` is mounted at `/system`. It is read-only and contains firmware-baked skills, skill assets, built-in Lua modules, Lua docs/tests, board image overlays, and `.recovery` seed files.
- `CLAW_PATH_DATA` is the writable storage root. It is `/fatfs` when flash storage is used, or the board-manager SD card mount point when an SD card is available.
- Never hard-code `/fatfs` for writable paths in reusable code or docs. Use `claw_paths_join(CLAW_PATH_DATA, ...)` in C and `storage.get_root_dir()` plus `storage.join_path(...)` in Lua.
- Firmware-baked skill scripts must be referenced with `{CUR_SKILL_DIR}/scripts/...` inside `SKILL.md`; do not write fixed `/fatfs/skills/...` paths.
- Runtime-installed/user skills live under the DATA root's `skills/`. Firmware-baked skills live under `/system/skills/`; the skill registry scans both, with DATA skills taking priority when ids conflict.
- Router rules, scheduler rules, memory, sessions, inbox, and user-generated files live under DATA. Recovery defaults are stored under `/system/.recovery` and copied into DATA only when missing.
- Built-in Lua libraries are staged under `/system/scripts/builtin/lib`; generated Lua module docs/tests are bundled into the `builtin_lua_modules` skill and should be accessed via that skill's `{CUR_SKILL_DIR}` paths.
- Board-specific `boards/<vendor>/<board>/fatfs_image/` content overlays the SYSTEM image at build time. Board image content does not target DATA and hidden board folders are not considered.

## Project-Specific Notes

- Architecture constraints: [`design.md`](.agents/design.md)
- docs guide: [`docs.md`](.agents/docs.md)
- Common gotchas: [`gotchas.md`](.agents/gotchas.md)
- Specs (`.agents/spec/`):
  - lua module spec: [lua-module-spec.md](.agents/spec/lua-module-spec.md)
  - claw skill spec: [claw-skill-spec.md](.agents/spec/claw-skill-spec.md)

## General Engineering Rules

- Use modular design. Each module should have clear responsibilities, ownership, and boundaries.
- Keep source files under 1500 lines where practical; split files by responsibility when they grow beyond that.
- Keep functions focused and reviewable; split large functions instead of adding deeply nested branches.
- Avoid magic numbers and magic strings. Use named constants, enums, macros, Kconfig options, or shared config keys.
- Prefer explicit ownership and explicit data flow over hidden global state.
- Keep public headers small and avoid exposing private implementation details.
- Avoid circular dependencies between components and modules.
- Check return values, handle allocation failures, and clean up partially initialized resources.
- Protect shared mutable state with documented ownership or synchronization.

## Code Style

- Implement the module in ESP-IDF using C-style object-oriented design, not C++.
- Represent each module as an object with an opaque handle: typedef struct xxx_t *xxx_handle_t.
- The header should expose only the handle, config, events, callbacks, and public APIs.
- Define struct xxx_t only in the .c file to store object state and resources.
- Use ESP-IDF-style APIs: xxx_create/delete/start/stop/read/write/set/get.
- Use xxx_handle_t handle as the first parameter of object methods.
- Prefer esp_err_t as the return type for public APIs.
- Use const xxx_config_t *config as create input and xxx_handle_t *ret_handle as output.
- Resources must be allocated in create and fully released in delete.
- Internal resources may include memory, GPIO, I2C, SPI, timers, tasks, queues, and mutexes.
- Protect shared state with mutexes or semaphores when accessed by multiple tasks.
- Register callbacks with xxx_register_cb(), using handle, event, and user_ctx.
- For polymorphism, use an xxx_ops_t function pointer table and put base struct as the first member.

## Memory Allocation and Release

- All runtime states must belong to a certain object instance.
- Avoid creating local variables larger than 128 bytes on task stacks; 
- Pre-allocated buffers, memory pools or ring buffers should be used in high-frequency scenarios.

## Testing

- Firmware changes should at minimum run `idf.py build` for the affected board configuration after exporting ESP-IDF and generating board manager config.
- Component test apps live under `components/claw_modules/*/test_apps/`.
- Lua module tests live beside modules under `components/lua_modules/<module>/test/` with descriptive names such as `json_roundtrip.lua`.
- Embedded frontend changes should run `cd application/edge_agent/components/http_server/frontend_source && pnpm build` and `pnpm typecheck`.

## Common File Locations

- App entry point: `application/edge_agent/main/main.c`
- Capability registration: `components/common/app_claw/app_capabilities.c`
- Lua module registration: `components/common/app_claw/app_lua_modules.c`
- App config schema/storage: `application/edge_agent/components/app_config/`
- Board definitions: `application/edge_agent/boards/`

## FT6336U 触控修复记录

### 根因
FT6336U 重置后需要 **更长启动时间** 才能响应寄存器读取。默认的 10ms 低 + 10ms 高延迟不足。

### 修复
`components/.../esp_lcd_touch_ft5x06.c` 中 `touch_ft5x06_reset()`：
- 低脉冲：10ms → **50ms**
- 释放后等待：10ms → **500ms**

### 诊断日志
修复前所有寄存器返回 0x00（包括 chip_id 寄存 0xA8）。修复后：
```
chip_id=0x11 firm_id=0xa3 lib_ver=5.1
```

### 改动文件
- `managed_components/espressif__esp_lcd_touch_ft5x06/esp_lcd_touch_ft5x06.c`：重置延迟提升，移除 debug 日志

---

## ESP-Claw music_player UI 修复记录

### 结论
launcher 标题改为支持 SKILL.md `title` 字段（多行中文，如"音乐\n播放器"），用独立 title_font（18-22px）+ line_space=-3 适配；网格改 2 行 × 3 列（title_h = short_side/6）。music_player 播放页标题仅显示文件名（basename）并用走马灯（`long_mode="scroll_circular"`）滚动过长名称；列表页文字加载 20px 字体。`lvgl.font_load()` 支持读取 `/system/fonts`（DATA root 找不到时 fallback 到 SYSTEM root）。

### 关键改动
1. **claw_skill**：`claw_skill_catalog_entry_t` 新增 `title` 字段（`components/claw_modules/claw_skill/src/claw_skill.c`），SKILL.md 可写自定义标题
2. **launcher**（`components/common/system_ui/src/launcher.c`）：`DEFAULT_ROWS 3→2`；title 优先用 entry->title；label 高度改 `LV_SIZE_CONTENT` + `system_ui_apply_title_font` + `line_space=-3`；`title_h = clamp(short_side/6, 40, 54)`
3. **system_ui_private.h**：新增 `title_font`（size = clamp(short_side/18, 18, 22)，独立于默认 font）与 `system_ui_apply_title_font()` inline
4. **lua_module_lvgl**：
   - `lua_lvgl_font.c`：`font_load()` 改用 VFS 读取（`CLAW_PATH_DATA` → NOT_FOUND 时 fallback `CLAW_PATH_SYSTEM`），字体数据存入 `record->data` 由 `lua_lvgl_release_font_record` 释放
   - `lua_lvgl_core_widgets.c` + `parse.c`：label 新增 `long_mode` 选项（`scroll_circular`/`scroll`/`wrap`/`dots`/`clip`）
   - `lua_lvgl_extra_widgets.c`：`list:add_button(text, symbol, font)` 新增可选第 3 个参数 font，应用到内部 label
5. **music_player main.lua**：`basename()` 函数；播放页 title 加 `long_mode="scroll_circular"` + 固定 h=40；列表页 `font_load("fonts/NotoSansSC-Regular-sub.ttf", {size=20})`；`ui.title:set_text(basename(playlist[idx].name))`

### 注意点
- **lv_tiny_ttf_create_data_ex 不复制数据**：字体 buffer 必须随 font 生命周期存活（default font 同 `s_lvgl.default_font_data` 做法）
- 走马灯需 label 有固定宽高（`w` + `h`）才会滚动
- `lv_list_add_button` 内部 label 默认已是 `LV_LABEL_LONG_MODE_SCROLL_CIRCULAR`（lv_list.c:102）
- fatfs_image 的 `storage/` 与 `system/.recovery/` 两副本必须 diff 一致；烧录新 system.bin 后需在设备运行 `lua --run --path /system/scripts/fix_main.lua` 同步 SD 卡副本
- 烧录时 system.bin（0x820000）含 `/system/.recovery` 的 main.lua，仅烧 edge_agent.bin 不会更新 SD 副本

### 验证结果（2026-08-01）
- `lua --run-async` 启动 main.lua 无 error，`default font loaded: fonts/NotoSansSC-Regular-sub.ttf` 正常
- system.bin 内 `function basename`/`scroll_circular`/`list_font` 关键字符串确认存在
- fix_main.lua 同步 SD 副本成功

---

## ESP-Claw LLM 连接与思考模型修复记录（2026-08-06）

### 结论
设备 AI 连接失败的根因是**双重的**：① Qwen3.5 思考模型的 `reasoning_content` 空响应判定 bug；② SD 卡接触不良导致回退 flash fatfs，agent 读记忆时 PSRAM 栈崩溃。

### 1. OpenAI 兼容后端 thinking 模型空响应 bug
- **症状**：Qwen3.5-4B 等思考模型响应格式为 `{reasoning_content:"思考", content:"答案"}`。当 `max_tokens` 不够时思考占满 token，`content` 为空 → 报错 "LLM returned empty text response"
- **修复**：`components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_openai_compatible.c:199` 空响应判定加入 `!out_response->reasoning_content`，与 Anthropic 后端（`claw_llm_backend_anthropic.c:658`）对齐

### 2. max_tokens 默认值提高
- `components/claw_modules/claw_core/src/llm/claw_llm_runtime.c:12`：`CLAW_LLM_DEFAULT_MAX_TOKENS` 8192 → **16384**
- `application/edge_agent/components/app_config/app_config.c:43`：`APP_DEFAULT_LLM_MAX_TOKENS` "8192" → **"16384"**
- 设备 NVS 已有旧值需通过 Web API 更新：`POST /api/config {"llm_max_tokens":"16384"}` + 重启

### 3. PSRAM 任务栈 + flash 读崩溃（重要）
- **症状**：`ask_once` 提交后立即崩溃：`assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())`
- **backtrace 链路**：`claw_core_agent_loop_task → claw_core_build_iteration_context → claw_memory_profile_collect → read_file_dup → fopen("/fatfs/memory/profile.json") → vfs_fat_open → f_open → wl_read → esp_partition_read → flash 禁缓存 → 断言`
- **根因**：`claw_core.c:228` agent 任务用 `stack_policy = CLAW_TASK_STACK_PREFER_PSRAM` 创建，栈在 PSRAM。当 storage 回退到 flash fatfs（`/fatfs`）时，agent 读记忆/写会话触发 flash 操作，禁缓存期间访问 PSRAM 栈 → 断言崩溃
- **触发条件**：**SD 卡挂载失败**（`sdmmc_init_sd_scr: send_scr returned 0x107`，`cmd=52/cmd=5` R1 全 1 = 卡无响应）→ storage 回退 `/fatfs` → 崩溃。SD 卡正常时记忆在 `/sdcard`，不读 flash，不崩溃
- **临时解决**：重新插拔 SD 卡恢复挂载（`storage_base_path` 回到 `/sdcard`）
- **固件层面待修**：agent 任务栈应改 `CLAW_TASK_STACK_INTERNAL_ONLY`（内部 RAM 充足 ~294KB），或做 flash 读取时保护。`claw_task.h` 已有 `CLAW_TASK_STACK_INTERNAL_ONLY` 枚举（claw_task.c 中 policy→`MALLOC_CAP_INTERNAL`）
- **相关配置**（`esp32s3_n16r8/sdkconfig.defaults.board`）：`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` + `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` 使 PSRAM 栈成为可能

### 4. 验证（2026-08-06）
- `ask_once "hi"` → 正常返回（context 加载 4 个 provider，completion done，约 4.5s）
- `ask_once "1+1等于几？"` → 正确返回 "2"
- 设备 IP：192.168.3.252，串口 /dev/ttyACM0（注意：之前是 /dev/ttyACM1，USB 枚举变化）

### 遗留（已解决）
- ~~agent 任务栈 PSRAM 崩溃尚未在固件层面修复~~ → 已修复，见下方"固件层修复"记录（2026-08-06 续）

### 5. PSRAM 栈任务固件层修复（2026-08-06 续）
- **原则**：凡任务内访问 DATA root 文件（读/写，SD 失效时即 flash）的任务，栈必须 `CLAW_TASK_STACK_INTERNAL_ONLY`，否则 flash 禁缓存期间访问 PSRAM 栈会断言崩溃
- **改动**（`PREFER_PSRAM` → `INTERNAL_ONLY`，全部已验证内部 RAM 充足 ~294KB）：
  - `components/claw_modules/claw_core/src/claw_core.c:228`：agent 任务（16KB）
  - `components/claw_modules/claw_memory/src/claw_memory_session.c:479`：`claw_mem_extract` 异步记忆提取（6KB）
  - `components/claw_modules/claw_event_router/src/claw_event_router.c:2382`：event_router（8KB，启动即读 router_rules.json，SD 失效时第一崩点）
  - `components/claw_capabilities/cap_scheduler/src/cap_scheduler.c:819`：cap_scheduler（6KB，读写 schedules.json）
- 未改：`cap_system_restart`（3KB，只重启不碰 flash）、IM 平台任务（网络为主，经 core API 间接访问存储）、`cap_lua_async`（已是 INTERNAL_ONLY）、`cap_system_time_sync`（已是 INTERNAL_ONLY）
- 验证：多次 ask_once（含中文）全部正常，无崩溃

### 6. NTP 时间同步失败 → TLS 失败根因（2026-08-06 续）
- **症状**：LLM 调用报 `HTTP request failed: ESP_ERR_HTTP_EAGAIN`，agent 卡在 LLM 请求；boot 日志 `Waiting for system time to be set... (N/15)` 持续失败
- **根因**：默认 NTP 服务器 `pool.ntp.org` + `time.windows.com` 在此网络环境**不可达**（PC 侧 python socket NTP 测试均 timeout）→ 系统时间永远不同步 → mbedTLS 验证服务器证书失败 → HTTPS 请求失败
- **验证**：`ntp.aliyun.com`、`cn.pool.ntp.org`、`ntp1.aliyun.com` 均可达（offset -1.2~-1.5s）
- **修复**：`components/claw_capabilities/cap_system/src/cap_system.c:40-41` 改为 `cn.pool.ntp.org` + `ntp.aliyun.com`
- **判定铁证**：LLM TLS 握手成功即证明时间已同步

## ESP-Claw RTHK HLS 网络收音机播放验证记录（2026-08-06）

### 结论
ESP32-S3 已能通过 audio_player（Lua）播放 RTHK HLS 直播流（master.m3u8 → index_64_a.m3u8 → 10s AAC-in-TS 切片），播放状态 `ESP_AUD_SIMPLE_PLAYER_RUNNING` 持续超过 30s 且跨切片滚动（`Parsed 3 segments` 多次）无错误。修复共 3 处（均为 managed_components 本地 patch，gitignore 未跟踪，**新克隆后需重新应用**）+ 1 处测试脚本修正。

### 修复清单
1. **io_hls 不可复制**（`audio_hls_io.c`）：`esp_gmf_pool_new_io` 调 `esp_gmf_obj_dupl` 要求 IO 有 `new_obj`；io_http 有（`obj->new_obj = _http_new`），io_hls 的 `obj->new_obj` 是 NULL → 报 `esp_gmf_obj_dupl is no new function [0x...-io_hls]`。修复：新增 `_hls_new`（透传 `esp_gmf_io_hls_init`）并赋值 `obj->new_obj = _hls_new`
2. **流水线 task 栈溢出**（`audio_player.c:196`）：`.task_stack = 0` → `16 * 1024`。`esp_gmf_io_open` → `_hls_open` → HLS TLS 下载 playlist 在流水线任务内**同步执行**，默认 `DEFAULT_ESP_GMF_STACK_SIZE`(4K) 溢出（`***ERROR*** A stack overflow in task`）
3. **HTTPS 证书**（`audio_simple_player_pool.c`）：HLS cfg 默认 `crt_bundle_attach=NULL` → `esp-tls-mbedtls: No server verification option set` → `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` → `ESP_ERR_HTTP_CONNECT`。修复：顶层 `#include "esp_crt_bundle.h"` + `hls_cfg.crt_bundle_attach = esp_crt_bundle_attach;`。**注意 include 必须在文件顶部**，放函数体内会触发 mbedtls 头文件 `invalid storage class` 编译错误
4. **测试脚本 lvgl 崩溃**（`test_radio.lua`）：`lvgl.process_events(100)` 在 lvgl 未 init 时报 `lvgl runtime is not initialized`（脚本 pcall 兜住后调 `player:close()` 导致看似"解码器关闭"）。修复：改用 `require("delay")` 的 `delay.delay_ms(100)`；state 字符串应为 `ESP_AUD_SIMPLE_PLAYER_RUNNING`（不是 `"playing"`），停止/错误为 `ESP_AUD_SIMPLE_PLAYER_STOPPED/ERROR`

### 播放链路（实际工作路径）
```
audio.player({output}) → player:play(master.m3u8) → pool io_hls → _hls_open
  → master→media playlist（TLS，crt_bundle）→ 切片入 ring(128KB)
  → aud_dec：TS_PARSER 检测 pmt type:15(ADTS AAC) pid:256 → 解码
  → aud_rate_cvt（`Not enough memory for out, need:4096, old:1024` 是正常扩容日志）
  → 内部 codec（24000Hz 1ch 16bit）
```
- 状态判定：`player:poll().state`，字符串见上；AUD_SIMP_PLAYER 正常 RUNNING 表示已出声
- 日志观察点：`Master playlist -> media playlist`、`Parsed 3 segments`、`HLS opened`、`TS_PARSER: pmt type:15`、`[t] PLAYING OK`

### 验证脚本与烧录
- `application/edge_agent/fatfs_image/system/scripts/test_radio.lua`：建 codec output + player → play RTHK master.m3u8 → 轮询 30s 找 RUNNING/STOPPED/ERROR → close
- 运行：`lua --run-async --path /system/scripts/test_radio.lua`；看结果用 `lua --job=<id>`（**不是** `lua_tail_async_job`）
- 改脚本只需重烧 system.bin（0xa20000）；改 C 需重烧 edge_agent.bin（0x20000）
- 串口：/dev/ttyACM1 打开即复位，用 `python3 /tmp/opencode/capture_radio.py` 捕获（自动复位+等 `app>`+发命令+存 `/tmp/opencode/radio_capture.log`）

### 已知问题（未修）
- HLS 首连偶发失败：`open ... failed: ESP_ERR_HTTP_CONNECT`（Wi-Fi 已连仍可能，重跑即恢复，疑似 TLS/网络瞬时）。无自动重试
- 未做：切片边缘无缝（live 断点续播）、错误自动重连、ICY/HTTP 流支持（audio_player.c 已有 http_stream ICY title 回调 + icy_name 字段代码，未提交）

### 相关文件
- `application/edge_agent/managed_components/espressif__gmf_io/src/audio_hls_io.c`（patch 1）
- `components/lua_modules/lua_module_audio/src/audio_player.c`（patch 2 + 未提交的 ICY/io_hls prev_cb 支持）
- `application/edge_agent/managed_components/espressif__esp_audio_simple_player/src/audio_simple_player_pool.c`（patch 3）
- `application/edge_agent/fatfs_image/system/scripts/test_radio.lua`、`http_test.lua`（连通性探针，用 cap `http_request` 拉 master.m3u8 返回 HTTP 200）

## ESP-Claw radio_player 停止/切台死锁修复记录（2026-08-08）

### 结论
radio skill"播放后不能停止、不能切台"的根因是 **UI 线程被 `player:poll()` 永久等锁冻结**。已修复并用户实测通过（正常停止 + 切台）。

### 根因
1. **主因（UI 冻结）**：`audio_player_out_cb`（`audio_player.c:154-165`）在 **持有 `player->lock` 期间**调用 `esp_codec_dev_write`（I2S DMA 写，缓冲满时阻塞）。radio skill 主循环（`main.lua:245-257`，单线程）执行 `check_playback_status() → player:poll()`，原实现 `xSemaphoreTake(player->lock, portMAX_DELAY)` 永久等同一把锁 → LVGL UI 线程冻结 → 停止/切台按钮点击全部无效
2. **次因（stop 阻塞）**：HLS 下载（`audio_hls_io.c` `hls_download`）的 `esp_http_client_open`/`fetch_headers`（TLS 握手）**不检查 `_is_abort`**，worker 卡死时 STOP_BIT 不设 → `esp_gmf_task_stop` 原实现 `GMF_TASK_WAIT_FOR_STATE_BITS(..., 0xFFFFFFFF)` **无限等待** → stop 永久阻塞

### 修复清单
1. **`audio_player.c:405-432`** `lua_audio_player_poll`：锁等待 `portMAX_DELAY` → `pdMS_TO_TICKS(500)`，超时返回 `"audio player: poll lock timeout"` 而非永久阻塞（**根因修复**）
2. **`audio_player.c:55-108`** 新增 `audio_player_prev_stop_cb`：`esp_gmf_pipeline_set_prev_stop_cb` 挂 abort 回调，stop 前 `esp_gmf_io_abort(in_io)` 中断输入 IO
3. **`esp_gmf_io.c`**（managed patch）三处：
   - `io_process_read:92`：read 返回 `ESP_GMF_IO_ABORT` 时映射为 `ESP_GMF_JOB_ERR_ABORT`（原被吞成 `FAIL`）
   - `esp_gmf_io_process:196-199`：ABORT 透传门槛加 `!io->_is_abort`（abort 是终止信号，不被 HOLD 吞掉）
   - `esp_gmf_io_abort:790-812`：不设 `_is_hold`（避免 worker 卡无界 HOLD_DONE 等待）+ 调用 `io->prev_close` 中断 TLS 握手阻塞
4. **`esp_gmf_task.c`**（managed patch）`esp_gmf_task_stop:720-735`：超时后不再无限等 STOP bit（原 `0xFFFFFFFF`），改为再等一个 `api_sync_time` 窗口，仍不到返回 `ESP_GMF_ERR_TIMEOUT`

### 持久化（重要）
- `esp_gmf_io.c` / `esp_gmf_task.c` 在 `managed_components/` 下**被 gitignore 不跟踪**，patch 已存于 **`patches/esp-gmf/`**（含 README，`patch -p1 --forward` 应用）。**新克隆/`idf.py fullclean` 后必须重新应用**（gmf_core v0.8.4，commit `5ef03925`，ESP-IDF v5.5.1）

### 验证
- 压测脚本（`test_radio_stress.lua`，8 轮跨 radio1/2/4）：每轮 stop 成功（45-1933ms），状态全 `ESP_AUD_SIMPLE_PLAYER_STOPPED`；每次 stop 前有 `HLS segment download failed: ESP_ERR_INVALID_STATE`（abort 生效）；脚本不再卡死（poll 死锁已解决）
- 用户实测：播放→停止→切台均正常响应

### 已知遗留
- HLS 首连偶发 `open ... failed: ESP_ERR_HTTP_CONNECT`（Wi-Fi 已连仍可能，重跑即恢复，疑似 TLS/网络瞬时，独立于 stop 问题）→ 播放器进入 `ESP_AUD_SIMPLE_PLAYER_ERROR`（非 RUNNING），无自动重试

## ESP-Claw LCD 180° 翻转与音量条边距调整记录（2026-08-08）

### 1. LCD 显示翻转 180°（esp32s3_n16r8 板）

**结论**：ST7796 320×480 竖屏整体翻转 180°（上下+左右），通过 `board_devices.yaml` 的 mirror 配置实现（MADCTL 层），触摸坐标同步软件翻转。已烧录实测生效。

**改动**（`application/edge_agent/boards/community/esp32s3_n16r8/board_devices.yaml`）：
- 显示 `display_lcd.config`：`mirror_x: true→false, mirror_y: false→true, swap_xy: false`（180° 翻转 = MADCTL 的 MX/MY 同时取反）
- 触摸 `lcd_touch.config.touch_config.flags` 新增：`swap_xy: false, mirror_x: true, mirror_y: true`（坐标做 180° 变换 `x'=x_max-x, y'=y_max-y`）

**机制**：
- panel 层：`dev_display_lcd.c:142` 调 `esp_lcd_panel_mirror(mirror_x, mirror_y)` → ST7789 驱动写 MADCTL 的 MX/MY 位（`esp_lcd_panel_st7789.c:247-261`）
- 触摸层：本项目自定义 FT5x06 驱动只设 `get_xy`（`esp_lcd_touch_ft5x06.c:121`），`set_mirror_*` 均 NULL → 标准库 `esp_lcd_touch_get_data`（`esp_lcd_touch.c:137-151`）走**软件坐标调整** `x_max - x` / `y_max - y`，仅配置 YAML flags 即可生效，无需改驱动
- SPI 显示分支固定 `ESP_LV_ADAPTER_ROTATE_0`（`display_service.c`），LVGL 层不参与旋转，方向完全由 MADCTL 控制，无双重旋转；LVGL 坐标系原点不变，UI 布局代码无需改动
- 翻转后坐标范围不变（0..319/0..479），x_max/y_max 无需改

**重新生成**：改 YAML 后需重新跑 `idf.py bmgr -c ./boards -b esp32s3_n16r8` 再 `idf.py build`；生成的 `components/gen_bmgr_codes/gen_board_device_config.c` 是 gitignore 产物，确认 `.mirror_x/mirror_y` 和 touch flags 正确即可

### 2. 音量条右端内缩（music_player + radio_player）

**结论**：两个 skill 播放页音量条右端太贴屏幕右沿，统一右端对齐到 x=280（右边距 40px）。

**改动**（每个文件有 `system/.recovery/` + `storage/` 两份，须 diff 一致）：
- `music_player/scripts/main.lua:296`：滑块 `x=96, w=196→184`（右端 292→280）
- `radio_player/scripts/main.lua:177`：滑块 `x=96, w=200→184`（右端 296→280）

**同步**：main.lua 烧入 system.bin（0xa20000）后，需设备运行 `lua --run --path /system/scripts/fix_main.lua` 将 `.recovery` 副本同步到 SD 卡（`/sdcard/skills/<skill>/scripts/`）

## ESP-Claw radio_player 电台扩展 + HLS URL 修复记录（2026-08-08）

### 结论
`stations.json` 从 9 台扩展到 **26 台**（8 RTHK + 13 CNR + 5 深圳本地），修正 CNR CDN 不可用问题，并修复 HLS IO 对**协议相对分片 URL**（`//host/path`）的拼接 bug（深圳蜻蜓 FM 直播依赖此修复）。设备实测 9 台代表全部 `RUNNING`。

### 电台清单（26 台）
- **RTHK 8**：第一~五台、普通话台、转播CNR香港之声（`rthkradiocnrhk`）、转播大湾区之声（`rthkradiocmgrgb`）
- **CNR 13**（全部走 `https://ngcdn002.cnr.cn/live/<id>/index.m3u8`）：中国之声 zgzs、经济之声 jjzs、音乐之声 yyzs、经典音乐广播 dszs（原误标"中国之声"，已修正）、台海之声 zhzs、神州之声 szzs、大湾区之声 hxzs、民族之声 mzzs、文艺之声 wyzs、老年之声 lnzs、香港之声 xgzs、中国交通广播 gsgljtgb、中国乡村之声 xczs
- **深圳 5**（蜻蜓 FM `https://ls.qingting.fm/live/<id>.m3u8`，HTTPS）：先锋898=1270、飞扬971=1271、快乐1062=1272、私家车94.2=1273、星光FM99.1=28132

### 源调研关键结论
- `ngcdn004.cnr.cn` **403 不可用** → 全部改 `ngcdn002.cnr.cn`（实测全部 200）
- 蜻蜓 FM 的 HLS 分片 URL 是**协议相对** `//ls-hw-ot.qtfm.cn/...`（PC curl 能跟但设备端 HLS 客户端需显式拼接）
- 深圳台名带后缀（如 `深圳先锋898(新闻广播)`），station 匹配用 `name:match("先锋898")` 而非精确相等
- **商业电台（881/903/864）不可静态收录**：CloudFront 动态签名 Cookie（Policy/Signature/Key-Pair-Id）+ IP 绑定 + playwright 级浏览器才拿得到；`radio.0472.org` 403 反盗链；SKILL.md 已注明
- **新城电台不可达**：`metroradio.com.hk` 网络超时
- 未收录藏/维/哈语台（用户决策）

### HLS URL 修复（关键代码改动）
**文件**：`application/edge_agent/managed_components/espressif__gmf_io/src/audio_hls_io.c`
**根因**：`hls_resolve_url()` 只处理绝对 URL（直接返回）和目录相对路径（`strrchr` 拼接）。`//host/...` 开头被当普通相对路径拼到 playlist 目录后 → `https://ls.qingting.fm/live//ls-hw-ot.qtfm.cn/...` → HTTP 客户端 `Error parse url` / `ESP_ERR_INVALID_RESPONSE`。
**修复**（协议相对 URL 分支）：
```c
if (segment[0] == '/' && segment[1] == '/') {
    const char *scheme_end = strstr(base_url, "://");
    size_t scheme_len = (size_t)(scheme_end - base_url) + 1; /* "https:" */
    memcpy(url, base_url, scheme_len);
    memcpy(url + scheme_len, segment, seg_len); /* segment 自带 "//host..." */
}
```
**坑**：scheme_len 若取 `+3`（含 `//`）会拼成 `https:////host` 仍错——必须只取 `scheme:`（`+1`），让 segment 自带的 `//` 补全主机分隔。
**持久化**：managed_components 被 gitignore，修复已存 `patches/esp-gmf/audio_hls_io.c`（**整文件备份**，因 Component Manager 缓存无原始 HLS 源文件，不能用 unified diff；含 `_hls_new` 透传 + `crt_bundle_attach` + 协议相对 URL 三处修改）。新克隆后 `cp patches/esp-gmf/audio_hls_io.c → managed_components/espressif__gmf_io/src/`。

### 播放测试脚本
`fatfs_image/system/scripts/test_radio_play.lua`（烧进 system.bin）：读 stations.json → 顺序播放 9 个代表台（RTHK 第一台基线 + 深圳 5 + CNR 中国之声/经典音乐广播 + RTHK 转播香港之声），每台等 RUNNING（≤12s）后 stop，统计结果。运行：
```
lua --run-async --path /system/scripts/test_radio_play.lua --timeout-ms 250000
lua --job=<id>   # 查看结果（不是 lua_get_async_job / --tail）
```
**验证结果**：设备硬复位重启后全部 RUNNING（RTHK 6.7s / 深圳 1.5-3.2s / CNR 2.4s / 转播 7.9s）。

### 坑与经验
- **设备长时间运行后播放器状态卡 NONE**：I2S 通道残留（`i2s_channel_disable: the channel has not been enabled`），所有台 HLS 打开 + 解码器 init 正常但 poll 不到 RUNNING。**esptool hard reset（`--after hard_reset run`）重启设备即恢复**，非固件 bug
- NTP 同步失败（`Waiting for system time...` UDP 123 被拦）不影响 HLS 播放（TLS 证书时间校验前已缓存），但会伴随网络波动表现
- `wifi --status`（不是 `wifi status`）查网络状态
- `lua --job=<id>` 返回 `status=done` + `summary=`（脚本 print 输出）+ `recent_log=`（滚动尾部），比串口日志可靠（async job 有 log_bytes=4096 环形缓冲）

### 相关文件
- `application/edge_agent/fatfs_image/system/.recovery/skills/radio_player/stations.json`（+ `storage/` 副本，diff 须一致）— **26 台**
- 同上 `SKILL.md`（+ `storage/` 副本）— 26 台说明 + 未收录注释
- `application/edge_agent/fatfs_image/system/scripts/test_radio_play.lua` — 播放测试脚本（新）
- `patches/esp-gmf/audio_hls_io.c` + `.h` — HLS URL 修复整文件备份（新）
- `patches/esp-gmf/README.md` — 更新 apply 说明
- 验证用 Python 脚本在 `/tmp/opencode/`（run_clean_test.py / get_job2.py 等，不入库）

## AGENTS.md Best-Practice Notes

Use this file as a compact router, not an encyclopedia.

- Keep instructions specific to this repository and this documentation workflow.
- Prefer exact file paths and commands over broad principles.
- Point agents to the right source files instead of duplicating long architecture explanations here.
- Document boundaries and exceptions explicitly, especially when "do not create a page by default" is the expected behavior.
- Update this guide when the docs workflow changes; stale agent docs are worse than missing prose.
