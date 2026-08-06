local capability = require("capability")
local ok, out, err = capability.call("http_request", {
    method = "GET",
    url = "https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8",
    timeout = 15000,
}, { source_cap = "lua_http_test" })
print(string.format("[ht] ok=%s out=%s err=%s", tostring(ok), tostring(out), tostring(err)))
