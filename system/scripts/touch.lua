-- Shared direct manipulation. Position is millipixels; velocity is millipixels/s.
-- No allocations or catalogue reads while tracking. Four slots are reused.
local function abs(x) if x < 0 then return -x end return x end
local function clamp(x,a,b) if x<a then return a elseif x>b then return b end return x end
local function read(service,key,fallback)
    local value,err=ignite.read(service,key)
    if err~=0 then return fallback end
    return value
end
local function label(key) return read('system.locale.read',key,key) end
local function make_scroll(draw,activate,refresh,row_height)
    row_height=row_height or 64
    local p,v,target,maximum=0,0,0,0
    local pointer,start_y,start_x,origin,last_y,last_at,last_motion,travel=nil,0,0,0,0,0,0,0
    local pressed=-1
    local header=false
    local reduced=read('system.settings.read','reduce_motion','0')=='1'
    local function paint() draw(p//1000,pressed,start_x) end
    local function limits(count)
        maximum=clamp(count*row_height-184,0,64000)*1000
        if p>maximum and pointer==nil then target=maximum end
    end
    local function event(kind,value,x,y,id,at)
        if kind=='focus_lost' or kind=='suspend' or kind=='touch_cancel' then
            pointer=nil; pressed=-1;v=0;target=clamp(p,0,maximum)
            if reduced then p=target;ignite.schedule(0) else ignite.schedule(33) end
            paint();return
        end
        if kind=='touch_down' then
            if pointer~=nil then return end
            header=y<56
            if y<56 then
                pointer=id;start_y=y;start_x=x;pressed=-2;travel=0;v=0
                paint();return
            end
            refresh();reduced=read('system.settings.read','reduce_motion','0')=='1'
            pointer=id;start_y=y;start_x=x;origin=p;last_y=y;last_at=at;last_motion=at;travel=0;v=0
            pressed=(p//1000+y-56)//row_height
            ignite.schedule(0);paint()
        elseif kind=='touch_move' and pointer==id then
            travel=abs(y-start_y)+abs(x-start_x)
            if header then if travel>8 then pressed=-1 end;paint();return end
            local next_p=origin-(y-start_y)*1000
            if next_p<0 then next_p=next_p//5 elseif next_p>maximum then next_p=maximum+(next_p-maximum)//5 end
            if at>last_at and y~=last_y then
                local sample=clamp(-(y-last_y)*1000000//(at-last_at),-1800000,1800000)
                v=(v+sample)//2;last_motion=at
            end
            last_at=at;last_y=y;p=next_p
            if travel>8 then pressed=-1 end
            paint()
        elseif kind=='touch_up' and pointer==id then
            pointer=nil
            local selected=pressed;pressed=-1
            if travel<=8 and abs(y-start_y)+abs(x-start_x)<=8 then
                paint()
                if selected==-2 and x<56 and y<56 then ignite.action('role','clock')
                elseif selected>=0 then activate(selected,x) end
                return
            end
            if at-last_motion>80 then v=0 end
            target=clamp(p+v*180//1000,0,maximum)
            if reduced then p=clamp(p,0,maximum);v=0;ignite.schedule(0)
            else ignite.schedule(33) end
            paint()
        end
    end
    local function update(dt)
        if pointer~=nil then return end
        -- Critically damped release, 0.4 s response: k=(2*pi/0.4)^2, c=2*sqrt(k).
        -- Four <=9ms substeps keep the fixed-point integrator stable under late frames.
        local step=clamp(dt,0,36)//4
        for i=1,4 do
            local acceleration=(-246740*(p-target)-31416*v)//1000
            v=v+acceleration*step//1000;p=p+v*step//1000
        end
        if abs(p-target)<500 and abs(v)<5000 then p=target;v=0;ignite.schedule(0) end
        paint()
    end
    return {event=event,update=update,limits=limits,paint=paint,
        seek=function(position) pointer=nil;pressed=-1;v=0;p=clamp(position*1000,0,maximum);target=p;ignite.schedule(0);paint() end}
end
