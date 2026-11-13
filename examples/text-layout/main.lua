local long = true
local message = "Датчики температуры и влажности недоступны. Служба остановлена."
ignite.value('message', message)
ignite.schedule(0)
return {
    update = function() end,
    event = function(kind)
        if kind == 'press' then
            long = not long
            ignite.value('message', long and message or 'Короткая строка')
        end
    end
}
