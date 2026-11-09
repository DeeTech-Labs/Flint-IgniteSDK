local count, read_error = ignite.storage_get('count', 0)
if count == nil then count = 0 end
local dirty = false
ignite.schedule(0)
ignite.value('count', count)
ignite.value('status', read_error == 0 and 'Restored' or 'Ready')

local function save()
    local error = ignite.storage_set('count', count)
    if error == 0 then
        dirty = false
        ignite.value('status', 'Saved')
    elseif error == 9 then
        ignite.value('status', 'Saving...')
        ignite.timer(1, 1000, false)
    else
        ignite.value('status', 'Save failed')
        ignite.log('Private storage write failed')
    end
end

return {
    update = function() end,
    event = function(kind, value)
        if kind == 'press' then
            count = count + 1
            dirty = true
            ignite.value('count', count)
            ignite.set(ui.indicator, 'color', 65535)
            save()
        elseif kind == 'timer' and value == 1 and dirty then
            save()
        elseif kind == 'resume' and dirty then
            save()
        end
    end
}
