local function read(key,fallback)
 local value,err=ignite.read('system.settings.read',key)
 if err~=0 then return fallback end
 return value
end
local function label(key)
 local value,err=ignite.read('system.locale.read',key)
 return err==0 and value or key
end
local cards={ui.card0,ui.card1,ui.card2,ui.card3}
local controls={ui.control0,ui.control1,ui.control2,ui.control3}
local values={ui.value0,ui.value1,ui.value2,ui.value3}
local pointer=nil
local selected,start_x,start_y=-1,0,0
local ble=false
local on,off=label('on'),label('off')
local function refresh()
 ble=read('ble_available',0)==1
 local radio=ble and read('ble_enabled','0')=='1'
 local reduced=read('reduce_motion','0')=='1'
 ignite.value('value0',ble and (radio and on or off) or label('ble_unavailable'))
 local offset=read('utc_offset_minutes',0)
 local sign=offset<0 and '-' or '+'
 if offset<0 then offset=-offset end
 local hours,minutes=offset//60,offset%60
 ignite.value('offset',sign..(hours<10 and '0' or '')..hours..':'..(minutes<10 and '0' or '')..minutes)
 ignite.value('value2',label('locale_label'))
 ignite.value('value3',reduced and on or off)
 ignite.set(controls[1],'color',radio and 0xfffe or 0xfffd)
 ignite.set(values[1],'color',not ble and 0xfff9 or radio and 0xfffd or 0xfffc)
 ignite.set(ui.title0,'color',ble and 0xfffc or 0xfff9)
 ignite.set(controls[1],'x',ble and 202 or 142);ignite.set(controls[1],'w',ble and 54 or 114)
 ignite.set(values[1],'x',ble and 229 or 199)
 ignite.set(controls[4],'color',reduced and 0xfffe or 0xfffd)
 ignite.set(values[4],'color',reduced and 0xfffd or 0xfffc)
end
local function hit(x,y)
 if x<8 or x>272 then return -1 end
 if y>=0 and y<56 then return x<56 and 4 or -1 end
 if y<56 or y>=238 then return -1 end
 local row=(y-56)//46
 return row==0 and not ble and -1 or row
end
local function paint()
 for i=1,4 do ignite.set(cards[i],'color',selected==i-1 and 0xfffa or 0xfffb) end
end
refresh();ignite.schedule(0)
return {
 update=function() refresh();ignite.schedule(0) end,
 event=function(kind,value,x,y,id)
  if kind=='touch_down' and pointer==nil then
   pointer=id;start_x=x;start_y=y;selected=hit(x,y);paint()
  elseif kind=='touch_move' and pointer==id then
   local dx,dy=x-start_x,y-start_y
   if dx*dx+dy*dy>64 or hit(x,y)~=selected then selected=-1;paint() end
  elseif kind=='touch_up' and pointer==id then
   pointer=nil
   local row=selected;selected=-1;paint()
   local dx,dy=x-start_x,y-start_y
   if row<0 or dx*dx+dy*dy>64 or hit(x,y)~=row then return end
   if row==4 then ignite.action('role','clock');return end
   local command=row==0 and 'ble_toggle' or row==1 and (x>=210 and 'offset_up' or x>=154 and 'offset_down' or '') or row==2 and 'language' or 'motion'
   if command~='' then ignite.action('system',command);ignite.schedule(1) end
  elseif kind=='touch_cancel' or kind=='focus_lost' or kind=='suspend' then
   pointer=nil;selected=-1;paint()
  end
 end
}
