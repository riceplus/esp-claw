local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")
local system = require("system")

local codec, rate, channels, bits = bm.get_audio_codec_output_params("audio_dac")
print("[v] params:", codec, rate, channels, bits)
local output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
print("[v] output:", output)
local player = audio.player({ output = output })
print("[v] player:", player)
local url = "https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8"
local ok, err = pcall(function() player:play(url) end)
print("[v] play:", ok, tostring(err))

local t0 = system.millis()
while (system.millis() - t0) < 45000 do
  local okp, s = pcall(function() return player:poll() end)
  if okp and s and s.state == "ESP_AUD_SIMPLE_PLAYER_RUNNING" then
    print("[v] RUNNING at " .. (system.millis()-t0) .. "ms")
    break
  end
  delay.delay_ms(300)
end
print("[v] state:", player:poll().state)

for _, v in ipairs({0, 50, 100, 0}) do
  local okv, errv = pcall(function() output:set_volume(v) end)
  print("[v] set_volume(" .. v .. ") ok=" .. tostring(okv) .. " err=" .. tostring(errv))
  local w0 = system.millis()
  while (system.millis() - w0) < 4000 do
    player:poll()
    delay.delay_ms(200)
  end
end
print("[v] done")
pcall(function() player:close() end)
pcall(function() output:close() end)
