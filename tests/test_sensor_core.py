"""Compile actual sensor applications and exercise Core with virtual monotonic time."""
from generated_paths import FACTORY_DIR
import copy
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project

MAIN = '''local h=0
local n=0
local _,e=ignite.sensors.subscribe(1,1,100)
ignite.value('load',e)
ignite.value('events',0)
ignite.value('result',0)
ignite.value('filtered',0)
ignite.schedule(0)
return {
 update=function(dt)
  if dt==0 then local _,e=ignite.sensors.subscribe(1,1,100); ignite.value('loading_update',e) end
 end,
 event=function(kind,v)
  if kind~='press' then return end
  if v==1 then local e; h,e=ignite.sensors.subscribe(1,1,100); ignite.value('result',e)
   if h then ignite.value('handle',h) end
  elseif v==2 then
   local s,e=ignite.sensors.read(1,1,false); ignite.value('result',e)
   if s and s.values and s.values.temperature and not s.values.humidity and not s.values.pressure then
    ignite.value('filtered',1)
   end
  elseif v==3 then local _,e=ignite.sensors.read(1,4,false); ignite.value('result',e)
  elseif v==4 then ignite.value('result',ignite.sensors.unsubscribe(h))
  elseif v==5 then ignite.value('result',ignite.brightness(90,5000))
  elseif v<0 then ignite.value('result',ignite.sensors.unsubscribe(-v))
  end
 end,
 sensor=function(handle,s,error,drops,finished)
  n=n+1
  ignite.value('events',n)
  if handle==h and error==0 and not finished and drops==0 and s.simulation and
     s.values.temperature.x==23.5 and not s.values.humidity and not s.values.pressure then
    ignite.value('filtered',1)
  else ignite.value('filtered',-1) end
 end
}
'''

def main():
    temporary = ROOT / "temp"
    temporary.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sensor-core-", dir=temporary) as directory:
        root = Path(directory)
        project = {
            'id': 'test.sensors',
            'name': 'Sensors',
            'publisher': 'tests',
            'version': 1,
            'components': [
                {
                    'id': 'main',
                    'type': 'app',
                    'ui': [
                        {
                            'kind': 'rect',
                            'w': 280,
                            'h': 240
                        },
                        {
                            'kind': 'text',
                            'text': 'load=${app.load};loading_update=${app.loading_update};result=${app.result};events=${app.events};filtered=${app.filtered};quota=${app.quota};handle=${app.handle};'
                        }
                    ],
                    'capabilities': 67,
                    'sensor_api': 1,
                    'app_api': 1,
                    'logic': {
                        'language': 'lua',
                        'path': 'main.lua'
                    },
                    'budget': {
                        'internal': 98304,
                        'instructions': 12000,
                        'external': 0
                    },
                    'permissions': [
                        'ui.render',
                        'input.focused',
                        'time.timer.create',
                        'sensors.temperature',
                        'ui.brightness'
                    ]
                }
            ]
        }
        def package(name, script, permissions=None):
            selected = copy.deepcopy(project)
            selected["id"] = "test." + name
            if permissions is not None:
                selected["components"][0]["permissions"] = permissions
            (root / "main.lua").write_text(script, encoding="utf-8")
            (root / (name + ".ignit")).write_bytes(compile_project(selected, root))
        package("sensors", MAIN)
        package("undeclared", MAIN, ["ui.render", "input.focused", "time.timer.create"])
        package("crash", '''ignite.schedule(0)
return {update=function() end,event=function() ignite.sensors.subscribe(1,1,100) end,
sensor=function() local x=nil; x=x+1 end}''')
        package("native", '''ignite.schedule(0)
ignite.value('quota',0)
return {update=function() end,event=function(kind,v)
 ignite.sensors.subscribe(1,1,100)
 if v==2 then ignite.sensors.subscribe(1,1,100) end
end,sensor=function()
 for i=1,20 do local _,e=ignite.sensors.read(1,1,false)
  if e==9 then ignite.value('quota',1) end
 end
end}''')
        package("instruction", '''ignite.schedule(0)
return {update=function() end,event=function(kind,v)
 ignite.sensors.subscribe(1,1,100)
 if v==2 then ignite.sensors.subscribe(1,1,100) end
end,sensor=function()
 local sum=0; for i=1,4000 do sum=sum+i end
 ignite.value('result',sum)
end}''')
        old = bytearray((root / "sensors.ignit").read_bytes())
        # Revision is the first uint32 after magic, version and header length.
        struct.pack_into("<I", old, 12, 0x20261112)
        (root / "old.ignit").write_bytes(old)
        subprocess.run([sys.argv[1], str(root), str(FACTORY_DIR)], check=True, timeout=20)

if __name__ == "__main__":
    main()
