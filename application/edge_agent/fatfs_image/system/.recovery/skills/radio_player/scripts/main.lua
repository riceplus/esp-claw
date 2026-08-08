local bm = require("board_manager")
local lvgl = require("lvgl")
local audio = require("audio")
local storage = require("storage")
local system = require("system")
local json = require("json")

local SKILL_DIR = "/sdcard/skills/radio_player"
local RECOVERY_DIR = "/system/.recovery/skills/radio_player"
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

local stations = {}
local current = nil
local output = nil
local player = nil
local is_playing = false

local ui = {}

local function set_ui_status(text)
  if ui.status then ui.status:set_text(text) end
end

local function read_stations()
  local path = SKILL_DIR .. "/stations.json"
  local ok, fh = pcall(io.open, path, "r")
  if not ok or not fh then
    path = RECOVERY_DIR .. "/stations.json"
    ok, fh = pcall(io.open, path, "r")
  end
  if not ok or not fh then
    print("[radio] no stations.json found")
    return
  end
  local data = fh:read("*a")
  fh:close()
  local okj, parsed = pcall(json.decode, data)
  if not okj or type(parsed) ~= "table" then
    print("[radio] stations.json parse error")
    return
  end
  stations = {}
  for _, s in ipairs(parsed) do
    if type(s) == "table" and s.name and s.url then
      table.insert(stations, { name = s.name, url = s.url, type = s.type or "http" })
    end
  end
end

local function highlight_list(idx)
  if not ui.station_items then return end
  for _, item in ipairs(ui.station_items) do
    if item.btn then
      item.btn:set_style({ bg_color = item.idx == idx and C.accent or C.card, bg_opa = item.idx == idx and 60 or 255 })
    end
  end
end

local function play_url(name, url)
  if not player then set_ui_status("播放器不可用"); return end
  local ok, err = pcall(function() player:stop() player:play(url) end)
  if not ok then
    print("play error: " .. tostring(err))
    set_ui_status("连接失败: " .. tostring(err))
    return
  end
  current = { name = name, url = url }
  is_playing = true
  if ui.title then ui.title:set_text(name) end
  if ui.song then ui.song:set_text("") end
  set_ui_status("播放中: " .. name)
end

local function play_idx(idx)
  if idx < 1 or idx > #stations then return end
  local s = stations[idx]
  play_url(s.name, s.url)
  highlight_list(idx)
end

local function play_manual()
  if not ui.url_input then return end
  local url = ui.url_input:get_text()
  if not url or url == "" then
    set_ui_status("请输入网址")
    return
  end
  play_url("手动频道", url)
end

local function stop_play()
  if not player then return end
  pcall(function() player:stop() end)
  is_playing = false
  current = nil
  if ui.title then ui.title:set_text("未在播放") end
  if ui.song then ui.song:set_text("") end
  set_ui_status("已停止")
end

local function check_playback_status()
  if not player or not is_playing then return end
  local ok, st = pcall(function() return player:poll() end)
  if not ok or not st then return end
  if st.icy_name and ui.song then
    local cur = ui.song:get_text()
    if cur ~= st.icy_name then
      ui.song:set_text(st.icy_name)
    end
  end
  if not st.running then
    is_playing = false
    set_ui_status("已停止")
  end
end

local function refresh_station_list()
  for _, item in ipairs(ui.station_items or {}) do
    if item.btn then pcall(function() item.btn:delete() end) end
  end
  ui.station_items = {}
  for i, s in ipairs(stations) do
    local btn = ui.list:add_button(s.name, nil, ui.list_font)
    btn:set_style({ bg_color = C.card, bg_opa = 255, text_color = C.text, radius = 8, pad = 8 })
    btn:on("clicked", function() play_idx(i) end)
    table.insert(ui.station_items, { btn = btn, idx = i })
  end
end

local function build_stations(tab)
  local hdr = lvgl.container(tab, { w = SCR_W, h = 44, bg_color = C.bg, pad = 0, border_width = 0 })
  lvgl.label(hdr, { text = "预设电台", align = "left_mid", x = 18, text_color = C.text })
  local refresh_btn = lvgl.button(hdr, { text = "刷新", align = "right_mid", x = -14, w = 60, h = 32, bg_color = C.card, radius = 16, text_color = C.sub })
  refresh_btn:on("clicked", function()
    read_stations()
    refresh_station_list()
    set_ui_status("已刷新")
  end)

  local ok_font, font = pcall(lvgl.font_load, "fonts/NotoSansSC-Regular-sub.ttf", { size = 20 })
  if ok_font and font then ui.list_font = font end

  ui.list = lvgl.list(tab, { align = "top_left", y = 44, w = SCR_W, h = BODY_H - 44, bg_color = C.bg, radius = 0, border_width = 0 })
  ui.list:set_style({ pad = 6, pad_row = 4 })
  ui.station_items = {}
  refresh_station_list()
end

local function build_now_playing(tab)
  ui.status = lvgl.label(tab, { text = "未在播放", align = "top_mid", y = 10, text_color = C.sub })

  ui.title = lvgl.label(tab, { text = "未在播放", align = "top_mid", y = 46, w = SCR_W - 24, h = 36, long_mode = "scroll_circular", text_color = C.text })

  lvgl.label(tab, { text = "现在播放", align = "top_left", x = 24, y = 104, text_color = C.sub })
  ui.song = lvgl.label(tab, { text = "", align = "top_left", x = 24, y = 132, w = SCR_W - 48, h = 80, long_mode = "wrap", text_color = C.accent })

  local stop_btn = lvgl.button(tab, { text = "停止", align = "top_mid", y = 240, w = 200, h = 48, bg_color = C.danger, radius = 24, text_color = "#ffffff" })
  stop_btn:on("clicked", stop_play)

  lvgl.label(tab, { text = "音量", align = "top_left", x = 24, y = 320, text_color = C.sub })
  local vol_slider = lvgl.slider(tab, { align = "top_left", x = 96, y = 324, w = 184, h = 20, min = 0, max = 100, value = 80, bg_color = C.ring, radius = 10 })
  if output then
    vol_slider:set_value(output:get_volume() or 80)
  end
  vol_slider:on("value_changed", function()
    local v = vol_slider:get_value()
    pcall(function() output:set_volume(v) end)
  end)
end

local function build_manual(tab)
  lvgl.label(tab, { text = "手动输入电台网址", align = "top_mid", y = 12, text_color = C.text })

  ui.url_input = lvgl.textarea(tab, { text = "http://", align = "top_mid", y = 48, w = SCR_W - 40, h = 44, bg_color = C.card2, text_color = C.text, border_color = C.ring })
  ui.url_input:set_style({ pad = 8 })

  local kbd = lvgl.keyboard(tab, { textarea = ui.url_input })
  kbd:set_style({ align = "bottom_left", y = 6 })

  local play_btn = lvgl.button(tab, { text = "播放", align = "top_mid", y = 104, w = 200, h = 44, bg_color = C.accent, radius = 22, text_color = "#ffffff" })
  play_btn:on("clicked", play_manual)

  lvgl.label(tab, { text = "支持 HLS (m3u8) 与直连 MP3/AAC", align = "top_mid", y = 160, w = SCR_W - 24, text_color = C.sub })
end

local function main()
  print("[radio] main() started")
  local panel, io, w, h, panel_if = bm.get_display_lcd_params("display_lcd")
  if not panel then error("get_display_lcd_params failed") end

  lvgl.init(panel, io, w, h, panel_if, { buffer_lines = 20, tick_ms = 5, task_period_ms = 10, font_size = 26, font_cache_size = 256 })
  print("[radio] lvgl.init ok")

  local touch, err = bm.get_lcd_touch_handle("lcd_touch")
  if touch then lvgl.indev_register("touch", touch) end

  local codec, rate, channels, bits = bm.get_audio_codec_output_params("audio_dac")
  if codec then
    output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
    if output then
      -- Boost top-end gain for radio only (sw_vol gain max ~+6dB; keep headroom)
      pcall(function() output:set_vol_curve({ { 0, -50 }, { 100, 5.5 } }) end)
      player = audio.player({ output = output })
    end
  end

  read_stations()

  local scr = lvgl.create_screen()
  scr:set_style({ bg_color = C.bg })

  local tv = lvgl.tabview(scr, { w = SCR_W, h = SCR_H, tab_bar_position = "bottom", tab_bar_size = TAB_BAR_H, bg_color = C.bg })
  tv:set_style({ bg_color = C.bg })

  local sta_tab = tv:add_tab("电台")
  local now_tab = tv:add_tab("播放")
  local man_tab = tv:add_tab("手动")

  sta_tab:set_scroll({ dir = "none", scrollbar = "off" })
  now_tab:set_scroll({ dir = "none", scrollbar = "off" })
  man_tab:set_scroll({ dir = "none", scrollbar = "off" })

  build_stations(sta_tab)
  build_now_playing(now_tab)
  build_manual(man_tab)

  scr:load()

  while true do
    local ok_loop, err_loop = pcall(function()
      lvgl.process_events(200)
      check_playback_status()
    end)
    if not ok_loop then
      if tostring(err_loop):match("stopped by user") then
        print("[radio] stopped")
        break
      end
      print("[radio] loop error: " .. tostring(err_loop))
    end
  end
end

local ok, err = xpcall(main, debug.traceback)
if output then pcall(function() output:set_vol_curve({ { 0, -50 }, { 100, 0 } }) end) end
if player then pcall(function() player:close() end) end
if output then pcall(function() output:close() end) end
pcall(lvgl.deinit)
if not ok then
  print("[radio] main error: " .. tostring(err))
end
