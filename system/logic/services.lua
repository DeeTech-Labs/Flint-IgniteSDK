local rows, selected, page, more = {}, 1, 0, false
local focus, pointer, pressed, pressed_action = 1, nil, -1, -1
local origin_x, origin_y, pressed_package, pressed_component = 0, 0, '', ''
local cards = {ui.card0, ui.card1, ui.card2}
local names = {ui.name0, ui.name1, ui.name2}
local states_ui = {ui.state0, ui.state1, ui.state2}
local buttons = {ui.button0, ui.button1, ui.button2, ui.button3}
local labels = {ui.action0, ui.action1, ui.action2, ui.action3}
local states = {[0]='Stopped',[1]='Starting',[2]='Running',[3]='Paused',[4]='Faulted'}
local reasons = {[0]='',[1]='User',[2]='Permission',[3]='Energy',[4]='Memory',[5]='Update',[6]='Error',[7]='Quota',[8]='Shutdown'}
local function command(slot)
 local row = rows[selected]
 if not row then return -1 end
 if slot == 4 then
  if row.state == 1 or row.state == 2 or row.state == 3 then return 1 end
  return row.execute_granted and 0 or -1
 elseif slot == 5 then return row.state == 2 and 2 or row.state == 3 and 3 or -1
 elseif slot == 6 then return row.execute_granted and (row.autostart and 5 or 4) or -1
 elseif slot == 7 then return row.execute_granted and 6 or -1 end
 return -1
end
local function paint()
 for i=1,3 do
  ignite.set(cards[i], 'color', selected==i and 0xfffe or focus==i and 0xfffa or 0xfffb)
 end
 local row=rows[selected]
 for i=1,4 do
  local action=command(i+3)
  local enabled=action>=0
  ignite.set(buttons[i], 'visible', enabled and 1 or 0)
  ignite.set(labels[i], 'visible', enabled and 1 or 0)
  ignite.set(buttons[i], 'color', (pressed==i+3 or focus==i+3) and 0xfffa or 0xfffb)
  local text=i==1 and (action==1 and 'Stop' or 'Start') or i==2 and (action==3 and 'Resume' or 'Pause') or i==3 and (row and row.autostart and 'Auto on' or 'Auto off') or 'Revoke'
  ignite.value('action'..(i-1),text)
 end
 ignite.set(ui.prev, 'visible', page>0 and 1 or 0)
 ignite.set(ui.next, 'visible', more and 1 or 0)
 ignite.set(ui.prev, 'color', focus==8 and 0xfffe or 0xfffc)
 ignite.set(ui.next, 'color', focus==9 and 0xfffe or 0xfffc)
 ignite.set(ui.back, 'color', focus==10 and 0xfffe or 0xfffc)
end
local function load_page()
 local result={}
 local failure=0
 for i=1,4 do
  local row,err=ignite.services.list(page*3+i-1)
  if i<=3 then result[i]=row end
  if i==4 then more=row~=nil end
  if i==1 then failure=err end
 end
 return result,failure
end
local function refresh()
 if not ignite.services then
  rows={};more=false;for i=1,3 do ignite.set(cards[i],'visible',0);ignite.set(names[i],'visible',0);ignite.set(states_ui[i],'visible',0) end;ignite.value('notice','Unavailable');paint();return
 end
 local previous=rows[selected]
 local result,err=load_page()
 if page>0 and not result[1] then page=0;result,err=load_page() end
 rows=result
 selected=rows[1] and 1 or 0
 if previous then
  for i=1,3 do
   local row=rows[i]
   if row and row.package==previous.package and row.component==previous.component then selected=i end
  end
 end
 for i=1,3 do
  local row=rows[i]
  ignite.set(cards[i], 'visible', row and 1 or 0)
  ignite.set(names[i], 'visible', row and 1 or 0)
  ignite.set(states_ui[i], 'visible', row and 1 or 0)
  ignite.value('name'..(i-1),row and row.label or '')
  local reason=row and reasons[row.reason] or ''
  ignite.value('state'..(i-1),row and ((states[row.state] or '?')..(reason~='' and ' / '..reason or '')) or '')
 end
 ignite.value('page',page+1)
 ignite.value('notice',rows[1] and '' or err==4 and 'Unavailable on this model' or 'No installed services')
 paint()
end
local function hit(x,y)
 if y<40 then
  if x<40 then return 10 end
  if x>=196 and x<236 and page>0 then return 8 end
  if x>=236 and more then return 9 end
 elseif y>=43 and y<169 and x>=8 and x<272 then
  local index=(y-43)//42+1
  return rows[index] and index or -1
 elseif y>=174 and y<205 and x>=8 and x<276 then
  local slot=(x-8)//67+4
  return command(slot)>=0 and slot or -1
 end
 return -1
end
local function capture(slot)
 pressed=slot;pressed_action=command(slot)
 local row=rows[selected]
 pressed_package=row and row.package or '';pressed_component=row and row.component or ''
 paint()
end
local function activate(slot)
 if slot>=1 and slot<=3 and rows[slot] then selected=slot;focus=slot;paint();return end
 if slot==8 and page>0 then page=page-1;refresh();return end
 if slot==9 and more then page=page+1;refresh();return end
 if slot==10 then ignite.action('role','launcher');return end
 local row=rows[selected]
 local action=command(slot)
 if action<0 or action~=pressed_action or not row or row.package~=pressed_package or row.component~=pressed_component then return end
 local err=ignite.services.control(action,row.package,row.component)
 refresh()
 if err~=0 then ignite.value('notice',err==5 and 'Permission denied' or err==4 and 'Unavailable' or 'Action failed: '..err) end
end
refresh();ignite.schedule(1000)
return {
 update=function() refresh();ignite.schedule(1000) end,
 event=function(kind,value,x,y,id)
  if kind=='touch_down' and pointer==nil then
   pointer=id;origin_x=x;origin_y=y;capture(hit(x,y))
  elseif kind=='touch_move' and pointer==id then
   local dx,dy=x-origin_x,y-origin_y
   if dx*dx+dy*dy>64 or hit(x,y)~=pressed then pressed=-1;paint() end
  elseif kind=='touch_up' and pointer==id then
   pointer=nil
   local slot=pressed;local dx,dy=x-origin_x,y-origin_y
   if slot>=0 and dx*dx+dy*dy<=64 and hit(x,y)==slot then activate(slot) end
   pressed=-1;paint()
  elseif kind=='rotate' then
   focus=focus+value;if focus<1 then focus=1 elseif focus>10 then focus=10 end;paint()
  elseif kind=='press' then
   if value==2 then focus=focus%10+1;pressed=-1;paint() else capture(focus) end
  elseif kind=='release' and value~=2 then
   local slot=pressed;if slot>=0 then activate(slot) end;pressed=-1;paint()
  elseif kind=='touch_cancel' or kind=='focus_lost' or kind=='suspend' then
   pointer=nil;pressed=-1;paint()
  end
 end
}
