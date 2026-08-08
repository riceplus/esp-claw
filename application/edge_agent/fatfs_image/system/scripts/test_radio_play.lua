local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")
local system = require("system")
local json = require("json")

local function read_stations()
  local paths = {
    "/sdcard/skills/radio_player/stations.json",
    "/system/.recovery/skills/radio_player/stations.json",
  }
  for _, p in ipairs(paths) do
    local ok, fh = pcall(io.open, p, "r")
    if ok and fh then
      local data = fh:read("*a")
      fh:close()
      local okj, parsed = pcall(json.decode, data)
      if okj and type(parsed) == "table" then
        local out = {}
        for _, s in ipairs(parsed) do
          if type(s) == "table" and s.name and s.url then
            table.insert(out, { name = s.name, url = s.url })
          end
        end
        return out
      end
    end
  end
  return nil
end

local stations = read_stations()
if not stations then
  print("[tr] no stations.json found")
  return
end
print("[tr] stations.json loaded, count=" .. #stations)

local test_pats = {
  { label = "RTHK radio1 (baseline)", pat = "香港电台第一台" },
  { label = "深圳先锋898", pat = "先锋898" },
  { label = "深圳飞扬971", pat = "飞扬971" },
  { label = "深圳快乐1062", pat = "快乐1062" },
  { label = "深圳私家车94.2", pat = "私家车94.2" },
  { label = "深圳星光FM99.1", pat = "星光FM99.1" },
  { label = "CNR 中国之声", pat = "中国之声" },
  { label = "CNR 经典音乐广播", pat = "经典音乐广播" },
  { label = "RTHK转播香港之声", pat = "转播CNR香港之声" },
}

local ok, codec, rate, channels, bits = pcall(bm.get_audio_codec_output_params, "audio_dac")
local output
if codec then
  output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
end
local player = output and audio.player({ output = output })
if not player then print("[tr] player unavailable"); return end

for _, t in ipairs(test_pats) do
  local found
  for _, s in ipairs(stations) do
    if s.name and s.name:match(t.pat) then found = s end
  end
  if not found then
    print("[tr] MISSING station for pattern: " .. t.pat)
  else
    print("[tr] === playing: " .. found.name)
    local okp, errp = pcall(function() player:play(found.url) end)
    if not okp then
      print("[tr] play call failed:", tostring(errp))
    else
      local t0 = system.millis()
      local state = "NONE"
      local result = "TIMEOUT"
      while true do
        local okst, st = pcall(function() return player:poll() end)
        if okst and st then state = st.state end
        if state == "ESP_AUD_SIMPLE_PLAYER_RUNNING" then
          print("[tr] RUNNING after " .. tostring((system.millis() - t0) / 1000) .. "s")
          result = "OK"
          break
        end
        if state == "ESP_AUD_SIMPLE_PLAYER_ERROR" or state == "ESP_AUD_SIMPLE_PLAYER_STOPPED" then
          print("[tr] failed state=" .. tostring(state))
          result = "FAIL"
          break
        end
        if (system.millis() - t0) > 12000 then
          print("[tr] timeout state=" .. tostring(state))
          result = "TIMEOUT"
          break
        end
        delay.delay_ms(100)
      end
      pcall(function() player:stop() end)
      delay.delay_ms(600)
    end
  end
end
print("[tr] all done")
pcall(function() player:close() end)
