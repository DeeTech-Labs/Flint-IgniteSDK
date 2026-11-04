-- Fixed-point coordinates keep the game independent of floating-point rounding.
local y, velocity, pipe, score, dead = 110000, 0, 260000, 0, false
local counted = false
return {
    update = function(dt, pressed)
        if dead and pressed then
            y, velocity, pipe, score, dead, counted = 110000, 0, 260000, 0, false, false
        end
        if not dead then
            if pressed then velocity = -160000 end
            velocity = velocity + 400 * dt
            y = y + velocity * dt // 1000
            pipe = pipe - 75 * dt
            if pipe < -24000 then pipe, counted = 280000, false end
            if pipe < 55000 and not counted then score, counted = score + 1, true end
            if y < 0 or y > 208000 or
                (pipe < 67000 and pipe + 24000 > 55000 and (y < 70000 or y + 12000 > 150000)) then
                dead = true
            end
        end
        ignite.set(2, 'y', y // 1000)
        ignite.set(3, 'x', pipe // 1000)
        ignite.set(4, 'x', pipe // 1000)
        ignite.value('score', score)
        ignite.value('dead', dead and 1 or 0)
    end
}
