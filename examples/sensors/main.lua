local handle=nil
ignite.value('temperature','unavailable')
ignite.value('error',0)
ignite.value('drops',0)
ignite.schedule(1000)
return {
 update=function(dt)
  if dt>0 and not handle then
   local e
   handle,e=ignite.sensors.subscribe(0,1,5000)
   ignite.value('error',e)
  end
 end,
 event=function(kind)
  if kind=='press' then
   local sample,e=ignite.sensors.read(0,1,false)
   ignite.value('error',e)
  end
 end,
 sensor=function(id,sample,e,drops,finished)
  ignite.value('error',e)
  ignite.value('drops',drops)
  if e==0 then
   ignite.value('temperature',''..sample.values.temperature.x)
  else
   ignite.value('temperature','unavailable')
  end
 end
}
