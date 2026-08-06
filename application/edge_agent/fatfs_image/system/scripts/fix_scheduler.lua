local s = require('storage')
local paths = {
    '/sdcard/scheduler/schedules.json',
    '/sdcard/scheduler/schedules.json.state',
    '/sdcard/scheduler/schedules.json.state.bak',
}
for _, p in ipairs(paths) do
    local ok, err = pcall(function() s.remove(p) end)
    if ok then
        print('removed ' .. p)
    else
        print('skip ' .. p .. ': ' .. tostring(err))
    end
end
