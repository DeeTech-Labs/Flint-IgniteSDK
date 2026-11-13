local function shared_read(key, fallback)
    local value, revision, error = ignite.state.read(key)
    if error ~= 0 or value == nil then return fallback, revision or 0, error end
    return value, revision, 0
end
local function status_label(value)
    if value == 0 then return 'fresh' end
    if value == 4 then return 'not supported' end
    if value == 5 then return 'permission denied' end
    if value == 7 then return 'source unavailable' end
    if value == 13 then return 'not ready' end
    return value
end
