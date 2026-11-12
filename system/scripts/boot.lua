local elapsed=0
local reduced=ignite.read('system.settings.read','reduce_motion')=='1'
-- Exact ease-out cubic Bezier (0.23,1,0.32,1), solved in fixed point.
local function ease(t)
    local lo,hi=0,1000
    for i=1,10 do
        local u=(lo+hi)//2;local m=1000-u
        local x=(3*m*m*u*230+3*m*u*u*320+u*u*u*1000)//1000000000
        if x<t then lo=u else hi=u end
    end
    local m=1000-(lo+hi)//2
    return 1000-m*m*m//1000000
end
return {
 update=function(dt)
    elapsed=elapsed+dt
    local t=reduced and 1000 or elapsed*1000//220
    if t>1000 then t=1000 end
    local shift=(1000-ease(t))*18//1000
    ignite.set(ui.stem,'x',126-shift)
    ignite.set(ui.top,'x',126+shift)
    ignite.set(ui.middle,'y',72+shift)
    ignite.set(ui.brand,'y',171+shift//2)
    ignite.schedule(t<1000 and 33 or 0)
 end,
 event=function(kind) if kind=='touch_up' then ignite.action('role','clock') end end
}
