local audio = require("audio")
local bm = require("board_manager")
local delay = require("delay")
local system = require("system")

local ok, codec, rate, channels, bits = pcall(bm.get_audio_codec_output_params, "audio_dac")
print("[t] codec params:", ok, tostring(codec), rate, channels, bits)
local output
if codec then
  output = audio.new_output({ codec, rate, channels, bits, volume = 80 })
  print("[t] output:", output)
end
local player = output and audio.player({ output = output })
print("[t] player:", player)
if not player then return end

local url = "https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8"
print("[t] playing:", url)
local okp, errp = pcall(function() player:play(url) end)
print("[t] play call:", okp, tostring(errp))

local t0 = system.millis()
local playing = false
while true do
  local okst, st = pcall(function() return player:poll() end)
  if okst and st then
    if st.state == "ESP_AUD_SIMPLE_PLAYER_RUNNING" and not playing then
      playing = true
      print("[t] PLAYING OK at " .. tostring((system.millis() - t0) / 1000) .. "s")
    end
    if st.state == "ESP_AUD_SIMPLE_PLAYER_STOPPED" then
      print("[t] STOPPED")
      break
    end
    if st.state == "ESP_AUD_SIMPLE_PLAYER_ERROR" then
      print("[t] ERROR state")
      break
    end
  end
  if (system.millis() - t0) > 30000 then
    print("[t] timeout 30s, state=" .. tostring(st and st.state))
    break
  end
  delay.delay_ms(100)
end
pcall(function() player:close() end)
print("[t] done")
