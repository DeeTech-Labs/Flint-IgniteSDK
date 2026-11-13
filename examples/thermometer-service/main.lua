local function refresh()
    local temperature = shared_read('temperature', 'unavailable')
    local humidity = shared_read('humidity', 'unavailable')
    local status = shared_read('status', 'Service stopped or permission required')
    ignite.value('temperature', temperature)
    ignite.value('humidity', humidity)
    ignite.value('status', status_label(status))
end
refresh()
return {
    update = function() end,
    state = function(revision) refresh() end,
    message = function(sender, topic, value)
        if sender == 'worker' and topic == 'status' then ignite.value('status', status_label(value)) end
    end,
    event = function(kind)
        if kind == 'press' then ignite.ipc.send('worker', 'refresh', true) end
    end
}
