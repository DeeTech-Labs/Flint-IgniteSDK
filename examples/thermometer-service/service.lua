local handle, samples = nil, 0
local last_flush = 0
local function publish(writes)
    local _, revision = shared_read('status', 7)
    local next_revision, error = ignite.state.commit(revision, writes)
    if error ~= 0 then ignite.ipc.send('main', 'status', error) end
    return error
end
local function subscribe()
    if handle then ignite.sensors.unsubscribe(handle); handle = nil end
    local error
    handle, error = ignite.sensors.subscribe(0, 5, 5000)
    if error ~= 0 then publish({status=error}) end
end
return {
    event = function(kind, reason)
        if kind == 'start' or kind == 'resume' then
            local now = ignite.read("time.monotonic")
            last_flush = now or 0
            subscribe()
            ignite.timer(1, 5000, true)
        elseif kind == 'pause' or kind == 'stop' then
            if handle then ignite.sensors.unsubscribe(handle); handle = nil end
        elseif kind == 'timer' and reason == 1 then
            local _, error = ignite.sensors.read(0, 5, false)
            if error == 5 or not handle then subscribe() end
        end
    end,
    sensor = function(id, sample, error, drops, finished)
        if id ~= handle then return end
        if error == 0 then
            -- Six paired readings in RAM; the complete bounded snapshot is flushed in batches.
            local next_slot = shared_read('history_next', 0)
            local slot = next_slot % 6
            local writes = {temperature=sample.values.temperature.x, humidity=sample.values.humidity.x,
                status=0, history_next=(slot+1)%6}
            writes['temperature_'..slot] = sample.values.temperature.x
            writes['humidity_'..slot] = sample.values.humidity.x
            publish(writes)
            samples = samples + 1
            local now, clock_error = ignite.read("time.monotonic")
            if samples >= 12 and clock_error == 0 and now - last_flush >= 60000 then
                local ticket, flush_error = ignite.state.flush()
                if flush_error == 0 then samples = 0; last_flush = now end
            end
        else
            publish({status=error})
        end
        if finished then handle = nil end
    end,
    message = function(sender, topic, value)
        if sender == 'main' and topic == 'refresh' then
            local _, error = ignite.sensors.read(0, 5, false)
            publish({status=error})
        end
    end,
    persisted = function(ticket, error)
        if error ~= 0 then publish({status=error}) end
    end,
    policy = function(value) if value then publish({status='energy policy paused'}) end end
}
