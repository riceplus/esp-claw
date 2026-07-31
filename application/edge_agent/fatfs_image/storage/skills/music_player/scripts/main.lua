local bm = require("board_manager")
local lvgl = require("lvgl")
local audio = require("audio")
local storage = require("storage")
local system = require("system")
local thread = require("thread")
local json = require("json")

local MUSIC_DIR = "/sdcard/music"
local AUDIO_EXTS = { ".mp3", ".wav", ".aac", ".ogg", ".flac", ".m4a", ".opus" }
local SCR_W, SCR_H = 320, 480
local TAB_BAR_H = 36
local BODY_H = SCR_H - TAB_BAR_H

local C = {
  bg     = "#0e0e1a",
  card   = "#1a1a2e",
  card2  = "#171728",
  ring   = "#1f1f38",
  accent = "#4f8cff",
  purple = "#a78bfa",
  text   = "#ececf4",
  sub    = "#8b8ba3",
  danger = "#f87171",
  ok     = "#34d399",
  warn   = "#fbbf24",
}

local playlist = {}
local current_idx = nil
local output = nil
local player = nil
local is_playing = false
local is_paused = false
local play_start_ms = 0
local pause_offset_ms = 0
local upload_svr_name = "music_upload"
local upload_svr_running = false

local ui = {}

local function fmt_time(ms)
  local t = math.max(0, math.floor(ms / 1000))
  return string.format("%02d:%02d", math.floor(t / 60), t % 60)
end

local function is_audio(name)
  local l = string.lower(name)
  for _, e in ipairs(AUDIO_EXTS) do
    if string.sub(l, -#e) == e then return true end
  end
  return false
end

local function utf8_trunc(s, max_chars)
  local i = 1
  local n = 0
  local len = #s
  while i <= len and n < max_chars do
    local b = string.byte(s, i)
    if b >= 0xF0 then i = i + 4
    elseif b >= 0xE0 then i = i + 3
    elseif b >= 0xC0 then i = i + 2
    else i = i + 1 end
    n = n + 1
  end
  if i <= len then return string.sub(s, 1, i - 1) .. ".." end
  return s
end

local function display_name(name)
  local dot = string.find(name, "%.", 1, true)
  local base = dot and string.sub(name, 1, dot - 1) or name
  return utf8_trunc(base, 18)
end

local function walk_music(dir, rel, out)
  local ok, entries = pcall(storage.listdir, dir)
  if not ok or not entries then return end
  for _, e in ipairs(entries) do
    if e.type == "dir" then
      local next_rel = rel == "" and e.name or rel .. "/" .. e.name
      walk_music(storage.join_path(dir, e.name), next_rel, out)
    elseif e.type == "file" and is_audio(e.name) then
      local rp = rel == "" and e.name or rel .. "/" .. e.name
      table.insert(out, { name = rp, path = storage.join_path(dir, e.name), display = display_name(rp) })
    end
  end
end

local function scan_music()
  playlist = {}
  walk_music(MUSIC_DIR, "", playlist)
  table.sort(playlist, function(a, b) return a.name < b.name end)
end

local function set_ui_status(text)
  if ui.status then ui.status:set_text(text) end
end

local function set_ui_elapsed(ms)
  if ui.elapsed then ui.elapsed:set_text(fmt_time(ms)) end
  if ui.progress then ui.progress:set_value(math.floor((math.max(0, ms) / 1000) % 600)) end
end

local function highlight_list(idx)
  if not ui.list_items then return end
  for _, item in ipairs(ui.list_items) do
    if item.btn then
      item.btn:set_style({ bg_color = item.idx == idx and C.accent or C.card, bg_opa = item.idx == idx and 60 or 255 })
    end
  end
end

local function play_idx(idx)
  if not player or idx < 1 or idx > #playlist then return end
  local ok, err = pcall(function() player:stop() player:play(playlist[idx].path) end)
  if not ok then print("play error: " .. tostring(err)) return end
  current_idx = idx
  is_playing = true
  is_paused = false
  play_start_ms = system.millis()
  pause_offset_ms = 0
  if ui.title then ui.title:set_text(playlist[idx].display) end
  if ui.pp then ui.pp:set_text("▶") end
  set_ui_status("播放中")
  highlight_list(idx)
  set_ui_elapsed(0)
end

local function toggle_play()
  if not player then return end
  if is_playing and not is_paused then
    local ok, err = pcall(function() player:pause() end)
    if ok then
      is_paused = true
      pause_offset_ms = system.millis() - play_start_ms
      if ui.pp then ui.pp:set_text("⏸") end
      set_ui_status("已暂停")
    end
  elseif is_playing and is_paused then
    local ok, err = pcall(function() player:resume() end)
    if ok then
      is_paused = false
      play_start_ms = system.millis() - pause_offset_ms
      if ui.pp then ui.pp:set_text("▶") end
      set_ui_status("播放中")
    end
  elseif #playlist > 0 then
    play_idx(current_idx or 1)
  end
end

local function stop_play()
  if not player then return end
  pcall(function() player:stop() end)
  is_playing = false
  is_paused = false
  play_start_ms = 0
  pause_offset_ms = 0
  if ui.pp then ui.pp:set_text("▶") end
  set_ui_status("待播放")
  set_ui_elapsed(0)
end

local PLAY_MODES = { "order", "loop", "single", "shuffle" }
local PLAY_MODE_LABEL = { order = "顺序", loop = "循环", single = "单曲", shuffle = "随机" }
local play_mode = "order"

local function next_song()
  if #playlist == 0 then return end
  local idx
  if play_mode == "shuffle" then
    if #playlist == 1 then
      idx = 1
    else
      repeat idx = math.random(#playlist) until idx ~= current_idx
    end
  else
    idx = current_idx and current_idx + 1 or 1
    if idx > #playlist then idx = 1 end
  end
  play_idx(idx)
end

local function prev_song()
  if #playlist == 0 then return end
  local idx = current_idx and current_idx - 1 or 1
  if idx < 1 then idx = #playlist end
  play_idx(idx)
end

local function cycle_mode()
  for i, m in ipairs(PLAY_MODES) do
    if m == play_mode then
      play_mode = PLAY_MODES[i % #PLAY_MODES + 1]
      break
    end
  end
  if ui.mode_btn then ui.mode_btn:set_text(PLAY_MODE_LABEL[play_mode]) end
  set_ui_status("播放模式: " .. PLAY_MODE_LABEL[play_mode])
end

local function check_playback_status()
  if not player or not is_playing then return end
  local ok, st = pcall(function() return player:poll() end)
  if not ok or not st then return end
  if st.state == "finished" or (not st.running and not is_paused) then
    if play_mode == "single" and current_idx then
      play_idx(current_idx)
    elseif play_mode == "order" and current_idx and current_idx >= #playlist then
      stop_play()
    else
      next_song()
    end
  end
  local elapsed = is_paused and pause_offset_ms or (system.millis() - play_start_ms)
  set_ui_elapsed(elapsed)
end

local function check_upload_status()
  local ok, call_ok, result = pcall(thread.list, "running")
  if not (ok and call_ok) then return end
  local found = false
  local name_marker = "name=" .. upload_svr_name
  for line in tostring(result):gmatch("[^\n]+") do
    if line:find(name_marker, 1, true) and line:find("| running |", 1, true) then
      found = true
      break
    end
  end
  if found ~= upload_svr_running then
    upload_svr_running = found
    if ui.svr_status then
      ui.svr_status:set_text(found and "服务器: 运行中" or "服务器: 已停止")
    end
    if ui.svr_status_bar then
      ui.svr_status_bar:set_style({ bg_color = found and C.ok or C.ring })
    end
    if ui.svr_btn then
      ui.svr_btn:set_text(found and "停止服务器" or "启动服务器")
    end
  end
end

local function start_upload_server()
  pcall(thread.stop, upload_svr_name, 1000)
  pcall(thread.stop, "upload_server.lua", 1000)
  local ok, result = pcall(thread.start, "/sdcard/skills/music_player/scripts/upload_server.lua", {}, { name = upload_svr_name, exclusive = "upload", replace = true })
  if not ok then print("start upload server failed: " .. tostring(result)) end
end

local function stop_upload_server()
  local ok, result = pcall(thread.stop, upload_svr_name, 3000)
  if not ok then print("stop upload server: " .. tostring(result)) end
  upload_svr_running = false
  if ui.svr_status then ui.svr_status:set_text("服务器: 已停止") end
  if ui.svr_status_bar then ui.svr_status_bar:set_style({ bg_color = C.ring }) end
  if ui.svr_btn then ui.svr_btn:set_text("启动服务器") end
end

local function toggle_upload_server()
  if upload_svr_running then stop_upload_server() else start_upload_server() end
end

local function build_now_playing(tab)
  local cover = lvgl.container(tab, { align = "top_mid", y = 8, w = 150, h = 150, bg_color = C.card2, radius = 75, pad = 0, border_width = 0 })
  ui.status = lvgl.label(cover, { text = "待播放", align = "center", text_color = C.sub })

  ui.title = lvgl.label(tab, { text = "请选择歌曲", align = "top_mid", y = 170, w = SCR_W - 32, text_color = C.text })

  ui.progress = lvgl.bar(tab, { align = "top_mid", y = 218, w = SCR_W - 40, h = 6, min = 0, max = 600, value = 0, bg_color = C.ring, radius = 3 })
  ui.progress:set_style({ bg_color = C.accent, bg_opa = 255 })

  ui.elapsed = lvgl.label(tab, { text = "00:00", align = "top_left", x = 20, y = 234, text_color = C.sub })
  lvgl.label(tab, { text = "10:00", align = "top_right", x = -20, y = 234, text_color = C.sub })

  local ctrl = lvgl.container(tab, { align = "top_mid", y = 280, w = SCR_W, h = 64, bg_color = C.bg, pad = 0, border_width = 0 })
  ctrl:set_flex({ flow = "row", main = "space_evenly", cross = "center" })

  lvgl.button(ctrl, { text = "⏮", w = 52, h = 52, bg_color = C.card, radius = 26, text_color = C.text }):on("clicked", prev_song)
  ui.pp = lvgl.button(ctrl, { text = "▶", w = 64, h = 64, bg_color = C.accent, radius = 32, text_color = "#ffffff" })
  ui.pp:on("clicked", toggle_play)
  lvgl.button(ctrl, { text = "⏭", w = 52, h = 52, bg_color = C.card, radius = 26, text_color = C.text }):on("clicked", next_song)
  lvgl.button(ctrl, { text = "⏹", w = 52, h = 52, bg_color = C.danger, radius = 26, text_color = "#ffffff" }):on("clicked", stop_play)
  ui.mode_btn = lvgl.button(ctrl, { text = PLAY_MODE_LABEL[play_mode], w = 48, h = 48, bg_color = C.card2, radius = 24, text_color = C.sub })
  ui.mode_btn:on("clicked", cycle_mode)

  lvgl.label(tab, { text = "音量", align = "top_left", x = 24, y = 374, text_color = C.sub })
  local vol_slider = lvgl.slider(tab, { align = "top_left", x = 96, y = 378, w = 196, h = 20, min = 0, max = 100, value = 80, bg_color = C.ring, radius = 10 })
  if output then
    vol_slider:set_value(output:get_volume() or 80)
  end
  vol_slider:on("value_changed", function()
    local v = vol_slider:get_value()
    pcall(function() output:set_volume(v) end)
  end)
end

local function build_playlist(tab)
  local hdr = lvgl.container(tab, { w = SCR_W, h = 44, bg_color = C.bg, pad = 0, border_width = 0 })
  lvgl.label(hdr, { text = "歌曲列表", align = "left_mid", x = 18, text_color = C.text })
  local refresh_btn = lvgl.button(hdr, { text = "刷新", align = "right_mid", x = -14, w = 60, h = 32, bg_color = C.card, radius = 16, text_color = C.sub })

  ui.list = lvgl.list(tab, { align = "top_left", y = 44, w = SCR_W, h = BODY_H - 44, bg_color = C.bg, radius = 0, border_width = 0 })
  ui.list:set_style({ pad = 6, pad_row = 4 })
  ui.list_items = {}

  local function refresh_list()
    for _, item in ipairs(ui.list_items or {}) do
      if item.btn then pcall(function() item.btn:delete() end) end
    end
    ui.list_items = {}
    scan_music()
    for i, song in ipairs(playlist) do
      local btn = ui.list:add_button(song.display)
      btn:set_style({ bg_color = (current_idx == i) and C.accent or C.card, bg_opa = (current_idx == i) and 60 or 255, text_color = C.text, radius = 8, pad = 8 })
      btn:on("clicked", function() play_idx(i) end)
      table.insert(ui.list_items, { btn = btn, idx = i })
    end
  end

  refresh_btn:on("clicked", refresh_list)
  refresh_list()
end

local function build_upload(tab)
  lvgl.label(tab, { text = "WiFi 上传", align = "top_mid", y = 10, text_color = C.text })

  ui.svr_status_bar = lvgl.container(tab, { align = "top_mid", y = 46, w = SCR_W - 40, h = 48, bg_color = C.ring, radius = 12, pad = 0 })
  ui.svr_status = lvgl.label(ui.svr_status_bar, { text = "服务器: 已停止", align = "center", text_color = C.text })

  ui.svr_btn = lvgl.button(tab, { text = "启动服务器", align = "top_mid", y = 108, w = 240, h = 44, bg_color = C.accent, radius = 22, text_color = "#ffffff" })
  ui.svr_btn:on("clicked", toggle_upload_server)

  local ip = system.ip() or "未连接"
  lvgl.label(tab, { text = "IP: " .. ip, align = "top_mid", y = 176, text_color = C.text })
  lvgl.label(tab, { text = "在电脑浏览器打开:", align = "top_mid", y = 212, text_color = C.sub })
  lvgl.label(tab, { text = "http://" .. ip .. "/api/lua/" .. upload_svr_name .. "/", align = "top_mid", y = 242, w = SCR_W - 24, text_color = C.accent })

  local ok, free = pcall(storage.get_free_space)
  if ok and free then
    lvgl.label(tab, { text = "存储: " .. tostring(math.floor(free.free / 1024)) .. "KB 可用", align = "top_mid", y = 300, text_color = C.sub })
  end
  lvgl.label(tab, { text = "上传的歌曲会保存到 /sdcard/music/", align = "top_mid", y = 340, w = SCR_W - 24, text_color = C.sub })
end

local function main()
  math.randomseed(system.millis())
  print("[music] main() started")
  local panel, io, w, h, panel_if = bm.get_display_lcd_params("display_lcd")
  if not panel then error("get_display_lcd_params failed") end
  print("[music] bm params ok")

  lvgl.init(panel, io, w, h, panel_if, { buffer_lines = 20, tick_ms = 5, task_period_ms = 10, font_size = 20, font_cache_size = 256 })
  print("[music] lvgl.init ok")

  local touch, err = bm.get_lcd_touch_handle("lcd_touch")
  print("[music] touch=" .. tostring(touch))
  if touch then lvgl.indev_register("touch", touch) end

  local codec, rate, channels, bits = bm.get_audio_codec_output_params("audio_dac")
  if codec then
    output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
    if output then player = audio.player({ output = output }) end
  end

  local ok, sd = pcall(storage.listdir, MUSIC_DIR)
  if not ok then pcall(storage.mkdir, MUSIC_DIR) end

  scan_music()

  local scr = lvgl.create_screen()
  scr:set_style({ bg_color = C.bg })

  local tv = lvgl.tabview(scr, { w = SCR_W, h = SCR_H, tab_bar_position = "bottom", tab_bar_size = TAB_BAR_H, bg_color = C.bg })
  tv:set_style({ bg_color = C.bg })

  local now_tab = tv:add_tab("播放")
  local list_tab = tv:add_tab("列表")
  local up_tab = tv:add_tab("上传")

  now_tab:set_scroll({ dir = "none", scrollbar = "off" })
  list_tab:set_scroll({ dir = "none", scrollbar = "off" })
  up_tab:set_scroll({ dir = "none", scrollbar = "off" })

  build_now_playing(now_tab)
  build_playlist(list_tab)
  build_upload(up_tab)

  scr:load()
  check_upload_status()

  while true do
    local ok_loop, err_loop = pcall(function()
      lvgl.process_events(200)
      check_playback_status()
      check_upload_status()
    end)
    if not ok_loop then
      print("[music] loop error: " .. tostring(err_loop))
    end
  end
end

local ok, err = xpcall(main, debug.traceback)
if player then pcall(function() player:close() end) end
if output then pcall(function() output:close() end) end
pcall(lvgl.deinit)
if not ok then
  print("[music] main error: " .. tostring(err))
end
