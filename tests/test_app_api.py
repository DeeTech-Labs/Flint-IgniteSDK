from generated_paths import FACTORY_DIR
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    project = {"id": "test.counter", "name": "Counter", "publisher": "tests", "version": 1,
        "app_api": 1, "capabilities": 67, "budget": {"internal": 49152, "instructions": 4096},
        "permissions": ["ui.render", "input.focused", "storage.private", "time.timer.create", "system.power.status", "ui.resources", "time.monotonic"],
        "logic": {"language": "lua", "path": "main.lua"},
        "components": [{"id": "main", "type": "app", "ui": [
            {"kind": "rect", "w": 280, "h": 240},
            {"kind": "rect", "id": "indicator", "w": 10, "h": 10},
            {"kind": "text", "text": "counter=${app.counter};text=${app.text};save=${app.save};timer=${app.timer};power=${app.power};"}]}]}
    project["resources"] = [{"id": i, "path": str(i) + ".bin"} for i in (100, 101, 102)]
    (root / "100.bin").write_bytes(struct.pack("<4sBBHHHIHB", b"FLIM", 2, 3, 1, 1, 0, 0, 65535, 255))
    (root / "101.bin").write_bytes(struct.pack("<4sBBHHHIHBHB", b"FLIM", 2, 3, 2, 1, 0, 0, 65535, 255, 0, 255))
    (root / "102.bin").write_bytes(b"not an image")
    project["components"][0]["ui"].append({"id": "icon", "kind": "image", "resource": 100, "w": 1, "h": 1})
    source = '''local n = ignite.storage_get('count',0)
ignite.schedule(0)
ignite.timer(1,100,false)
ignite.value('counter',n)
ignite.value('timer',0)
ignite.value('text','Привет')
local _,e=ignite.read('system.power.status','battery_percent'); ignite.value('power',e)
ignite.set(ui.indicator,'color',65535)
ignite.set(ui.indicator,'visible',0)
ignite.value('image',ignite.set(ui.icon,'image',101))
ignite.value('bad_image',ignite.set(ui.icon,'image',102))
return {update=function() end,event=function(kind,value)
  if kind=='press' then
    n=n+1; ignite.value('counter',n)
    ignite.value('save',ignite.storage_set('count',n))
  elseif kind=='timer' then ignite.value('timer',value)
  elseif kind=='release' then local t=ignite.read('time.monotonic'); ignite.value('power',t) end
end}
'''
    def package(name, source):
        (root / "main.lua").write_text(source, encoding="utf-8")
        path = root / (name + ".ignit")
        path.write_bytes(compile_project(project, root))
        return path
    counter = package("counter", source)
    project["components"][0]["ui"].pop()
    project["components"][0]["ui"][2]["text"] = "denied=${app.denied};"
    # A capability bit, rather than a format/revision bump, gates these additive APIs.
    project["id"] = "test.denied"
    project["permissions"] = ["ui.render"]

    denied = package("denied", "local _,e=ignite.read('time.monotonic'); ignite.value('denied',e); return {update=function() end}")
    project["id"] = "test.fault"
    project["permissions"] = ["ui.render", "input.focused"]
    fault = package("fault", "ignite.log('before failure'); return {update=function() end,event=function() while true do end end}")
    legacy = json.loads((ROOT / "examples/flappy-bird/project.json").read_text())
    (root / "main.lua").write_bytes((ROOT / "examples/flappy-bird/main.lua").read_bytes())
    legacy_path = root / "legacy.ignit"; legacy_path.write_bytes(compile_project(legacy, root))
    subprocess.run([sys.argv[1], str(root / "store"), str(FACTORY_DIR),
        str(counter), str(denied), str(fault), str(legacy_path)], check=True, timeout=10)
