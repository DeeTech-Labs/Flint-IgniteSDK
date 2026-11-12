from generated_paths import FACTORY_DIR
import json
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    project = json.loads((ROOT / "examples/flappy-bird/project.json").read_text())
    (root / "main.lua").write_bytes((ROOT / "examples/flappy-bird/main.lua").read_bytes())
    (root / "game.ignit").write_bytes(compile_project(project,root))
    (root / "main.lua").write_text("return {update=function(dt) if dt>0 then while true do end end end}")
    project["permissions"] = ["ui.render"]
    (root / "fault.ignit").write_bytes(compile_project(project,root))
    (root / "main.lua").write_text("return {update=function() ignite.set(2,'x',10); ignite.value('score',7) end}")
    (root / "quiet.ignit").write_bytes(compile_project(project,root))
    subprocess.run([sys.argv[1],str(root / "store"),str(FACTORY_DIR),
                    str(root / "game.ignit"),str(root / "fault.ignit"),str(root / "quiet.ignit")], check=True, timeout=5)
