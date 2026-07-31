local storage = require("storage")
local thread = require("thread")

local function copy(src_path, dst_path)
  local src = assert(io.open(src_path, "r"))
  local data = src:read("*a")
  src:close()
  local dst = assert(io.open(dst_path, "w"))
  dst:write(data)
  dst:close()
  print(src_path .. " -> " .. dst_path .. " OK")
end

local function mkdirs(path)
  local acc = ""
  for seg in path:gmatch("[^/]+") do
    acc = acc .. "/" .. seg
    pcall(storage.mkdir, acc)
  end
end

pcall(thread.stop, "upload_server.lua", 3000)
pcall(thread.stop, "music_upload", 3000)

local ok, call_ok, out = pcall(thread.list, "running")
print("pcall ok=" .. tostring(ok) .. " call_ok=" .. tostring(call_ok))
print("OUTPUT>>>" .. tostring(out) .. "<<<")

local sok, sent = pcall(storage.listdir, "/sdcard")
print("sdcard listdir ok=" .. tostring(sok))
if sok and sent then
  for _, e in ipairs(sent) do
    print("sdcard/: " .. e.name .. " (" .. e.type .. ")")
  end
end

mkdirs("/sdcard/skills/music_player/scripts")

copy("/system/.recovery/skills/music_player/scripts/main.lua",
     "/sdcard/skills/music_player/scripts/main.lua")
copy("/system/.recovery/skills/music_player/scripts/upload_server.lua",
     "/sdcard/skills/music_player/scripts/upload_server.lua")
