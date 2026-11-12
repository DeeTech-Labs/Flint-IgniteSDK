local cards={ui.card0,ui.card1,ui.card2,ui.card3,ui.card4,ui.card5}
local names={ui.name0,ui.name1,ui.name2,ui.name3,ui.name4,ui.name5}
local icons={ui.icon0,ui.icon1,ui.icon2,ui.icon3,ui.icon4,ui.icon5}
local targets={'','','','','',''}
local cached={-1,-1,-1,-1,-1,-1}
local count,top_cache,revision=0,-999,-1
local focused=-1
local scroll
local function refresh()
    local current=read('packages.list','revision',-1)
    if current==revision then return end
    revision=current
    count=read('packages.list','count',0);ignite.value('count',count);top_cache=-999
    if scroll then scroll.limits((count+1)//2) end
end
local function draw(position,selected,press_x)
    local top=position//100;if top<0 then top=0 end
    local remainder=position-top*100
    local column=press_x<140 and 0 or 1
    for i=1,6 do
        local index=top*2+i-1;local y=60+(i-1)//2*100-remainder
        local visible=index<count and y<240 and y+94>56
        ignite.set(cards[i],'visible',visible and 1 or 0);ignite.set(names[i],'visible',visible and 1 or 0);ignite.set(icons[i],'visible',visible and 1 or 0)
        if visible then
            ignite.set(cards[i],'y',y);ignite.set(names[i],'y',y+77);ignite.set(icons[i],'y',y+12)
            ignite.set(cards[i],'color',(selected>=0 and selected*2+column==index or selected<0 and focused==index) and 0xfffa or 0xfffb)
            if top~=top_cache or cached[i]~=index then
                cached[i]=index
                local name,target,err=ignite.catalog(index)
                ignite.value('row'..(i-1),err==0 and name or '')
                ignite.set(icons[i],'catalog',index)
                targets[i]=err==0 and target or ''
            end
        end
    end
    top_cache=top
    ignite.set(ui.empty,'visible',count==0 and 1 or 0);ignite.set(ui.empty_hint,'visible',count==0 and 1 or 0)
    local total=(count+1)//2*100;local thumb=total>184 and 164*184//total or 164
    if thumb<24 then thumb=24 end
    local range=total-184
    local y=64+(range>0 and clamp(position,0,range)*(164-thumb)//range or 0)
    ignite.set(ui.thumb,'y',y);ignite.set(ui.thumb,'h',thumb)
    ignite.set(ui.track,'visible',count>2 and 1 or 0);ignite.set(ui.thumb,'visible',count>2 and 1 or 0)
end
local function activate(row,x)
    local slot=(row-top_cache)*2+(x<140 and 1 or 2)
    if slot>=1 and slot<=6 and targets[slot]~='' then ignite.action('launch',targets[slot]) end
end
scroll=make_scroll(draw,activate,refresh,100)
refresh();scroll.limits((count+1)//2);scroll.paint();ignite.schedule(0)
return {
 update=function(dt) scroll.update(dt) end,
 event=function(kind,value,x,y,id,at)
    if kind=='catalog_changed' then focused=-1;refresh();scroll.paint()
    elseif kind=='rotate' and count>0 then
        focused=clamp((focused<0 and 0 or focused)+value,0,count-1)
        scroll.seek(focused//2*100)
    elseif kind=='press' and count>0 then
        if focused<0 then focused=0 end
        scroll.seek(focused//2*100)
    elseif kind=='release' and focused>=0 then
        activate(focused//2,focused%2==0 and 70 or 206)
    elseif kind=='focus_lost' then focused=-1;scroll.event(kind,value,x,y,id,at)
    else if kind=='touch_down' then focused=-1 end;scroll.event(kind,value,x,y,id,at) end
 end
}
