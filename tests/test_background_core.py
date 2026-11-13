"""Real FLM3 packages for background execution, isolation and bounded scheduling."""
from generated_paths import FACTORY_DIR
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project

UI = '''local old=0
local messages=0
ignite.schedule(0)
ignite.value('result',0); ignite.value('read',0); ignite.value('messages',0)
return {update=function() end,event=function(kind,v)
 if kind~='press' then return end
 if v==1 then local n,r,e=ignite.state.read('count'); ignite.value('read',n or -1); ignite.value('result',e)
 elseif v==2 then local r,e=ignite.state.commit(0,{guard=111}); ignite.value('result',e)
 elseif v==3 then ignite.value('result',ignite.ipc.send('worker','command',1))
 elseif v==4 then ignite.state.commit(0,{atomic=1,bad={}})
 elseif v==5 then local n,r,e=ignite.state.read('atomic'); ignite.value('result',e)
 elseif v==6 then local rejected=0; for i=1,12 do local e=ignite.ipc.send('worker','command',1); if e==9 then rejected=rejected+1 end end; ignite.value('read',rejected)
 elseif v==7 then local t,e=ignite.state.flush(); ignite.value('result',e)
 elseif v==8 then local h,r,e=ignite.state.read('handle'); if h then ignite.value('result',ignite.sensors.unsubscribe(h)) end
 elseif v==9 then local n,r,e=ignite.state.read('flush_error'); ignite.value('read',n or -1); ignite.value('result',e)
 elseif v==10 then ignite.value('result',ignite.ipc.send('worker','command',3))
 elseif v==11 then ignite.value('result',ignite.ipc.send('worker','command',2))
 elseif v==12 then ignite.value('result',ignite.ipc.send('worker','command',4))
 elseif v==13 then ignite.value('result',ignite.ipc.send('worker','command',5))
 elseif v==14 then ignite.value('result',ignite.ipc.send('foreign','command',1))
 elseif v==15 then old=ignite.state.read('handle') or 0
 elseif v==16 then ignite.value('result',ignite.ipc.send('worker','old',old))
 elseif v==17 then local n,r,e=ignite.state.read('old_error'); ignite.value('read',n or -1); ignite.value('result',e)
 elseif v==22 then local s,e=ignite.sensors.read(1,1,false); ignite.value('result',e)
 end
end,message=function(sender,topic,v) messages=messages+1; ignite.value('messages',messages) end,
persisted=function(ticket,e) ignite.value('result',e) end}
'''
SERVICE = '''local count=0
local handle=nil
local function save(writes)
 local _,revision,e=ignite.state.read('count')
 return ignite.state.commit(revision or 0,writes)
end
return {event=function(kind,v)
 if kind=='start' or kind=='resume' then
  handle=ignite.sensors.subscribe(1,1,100)
 elseif kind=='stop' or kind=='pause' then
  if handle then ignite.sensors.unsubscribe(handle); handle=nil end
 end
end,sensor=function(h,s,e)
 if e==0 then count=count+1; save({count=count,handle=handle}) end
end,message=function(sender,topic,v)
 if topic=='old' then save({old_error=ignite.sensors.unsubscribe(v)})
 elseif topic=='command' then
  if v==1 then ignite.ipc.send('main','reply',42)
  elseif v==2 then for i=1,100 do ignite.state.read('count') end
  elseif v==3 then while true do end
  elseif v==4 then local t={}; for i=1,500 do t[i]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} end
  elseif v==5 then local t,e=ignite.state.flush(); save({flush_error=e}) end
 end
end}
'''
BUSY = '''return {event=function(kind)
 if kind=='start' then ignite.ipc.send('worker','work',1) end
end,message=function(sender,topic,value)
 local sum=0; for i=1,100 do sum=sum+i end
 ignite.ipc.send('worker','work',value)
end}'''

def main():
    (ROOT / "temp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="background-core-", dir=ROOT / "temp") as directory:
        root = Path(directory)
        ui = {"id": "main", "type": "app", "app_api": 1, "sensor_api": 1, "ipc_api": 1,
            "capabilities": 65, "budget": {"internal": 65536, "external": 0, "instructions": 8192},
            "permissions": ["ui.render", "input.focused", "time.timer.create", "app.ipc", "storage.private"],
            "logic": {"language": "lua", "path": "ui.lua"},
            "ui": [{"kind": "rect", "w": 280, "h": 240},
                {"kind": "text", "text": "result=${app.result};read=${app.read};messages=${app.messages};"}]}
        service = {"id": "worker", "type": "service", "app_api": 1, "sensor_api": 1, "ipc_api": 1,
            "capabilities": 64, "budget": {"internal": 49152, "external": 0, "instructions": 4096},
            "permissions": ["app.background.execute", "app.background.autostart", "sensors.temperature",
                "app.ipc", "storage.private", "time.timer.create"],
            "logic": {"language": "lua", "path": "service.lua"}}
        (root / "ui.lua").write_text(UI)
        (root / "service.lua").write_text(SERVICE)
        def emit(name, version=1, busy=False):
            selected = copy.deepcopy(service)
            components = [copy.deepcopy(ui), selected]
            if busy:
                selected.pop("sensor_api"); selected["permissions"].remove("sensors.temperature")
                selected["logic"]["path"] = "busy.lua"; components = [selected]
            project = {"id": "test." + name, "name": name, "publisher": "tests", "version": version,
                "components": components}
            suffix = "-v2" if version == 2 else ""
            (root / (name + suffix + ".ignit")).write_bytes(compile_project(project, root))
        (root / "busy.lua").write_text(BUSY)
        emit("a"); emit("a", 2); emit("b"); emit("busy-a", busy=True); emit("busy-b", busy=True)
        example = ROOT / "examples/thermometer-service"
        example_project = json.loads((example / "project.json").read_text())
        (root / "example.ignit").write_bytes(compile_project(example_project, example))
        for name in ("pool-a", "pool-b"):
            selected = copy.deepcopy(example_project); selected["id"] = "test." + name
            (root / (name + ".ignit")).write_bytes(compile_project(selected, example))
        for name, event in (("pause-fault", "pause"), ("resume-fault", "resume"), ("stop-fault", "stop")):
            selected = copy.deepcopy(service)
            script = SERVICE.replace("if kind=='start' or kind=='resume' then",
                "if kind=='" + event + "' then while true do end elseif kind=='start' or kind=='resume' then")
            (root / (name + ".lua")).write_text(script)
            selected["logic"]["path"] = name + ".lua"
            project = {"id": "test." + name, "name": name, "publisher": "tests", "version": 1,
                "components": [copy.deepcopy(ui), selected]}
            (root / (name + ".ignit")).write_bytes(compile_project(project, root))
        subprocess.run([sys.argv[1], str(root), str(FACTORY_DIR)], check=True, timeout=40)

if __name__ == "__main__": main()
