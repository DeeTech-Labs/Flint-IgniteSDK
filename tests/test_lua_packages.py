import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK, RUN = sys.argv[1:]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project
from build_package import build

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    project = json.loads((ROOT / "examples/flappy-bird/project.json").read_text())
    (root / "main.lua").write_bytes((ROOT / "examples/flappy-bird/main.lua").read_bytes())
    package = root / "game.ignit"
    def check(success):
        package.write_bytes(compile_project(project, root))
        result = subprocess.run([CHECK, str(package)], capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == success, result.stderr
    check(True)
    canonical = package.read_bytes()
    source = (root / "main.lua").read_text(encoding="utf-8").encode("utf-8")
    (root / "main.lua").write_bytes(source.replace(b"\n", b"\r\n"))
    assert compile_project(project, root) == canonical, "Lua package depends on host line endings"
    (root / "main.lua").write_bytes(source)
    results = []
    for i in range(2):
        image = root / f"game{i}.ppm"
        result = subprocess.run([RUN, str(package), "120", "24", str(image)],
                                capture_output=True, text=True, check=True, timeout=5)
        report = json.loads(result.stdout)
        assert 0 < report["lua_peak_bytes"] <= 32768
        assert image.read_bytes().startswith(b"P6\n280 240\n255\n")
        results.append(report)
    assert results[0] == results[1], "simulation must be deterministic"
    for source in ("while true do end", "return {}", "io.open('x')",
                   "return {update=function() while true do end end}",
                   "local t={}; for i=1,100000 do t[i]=i end"):
        (root / "main.lua").write_text(source)
        check(False)
    project["components"][0]["capabilities"] = 1
    try: compile_project(project, root)
    except ValueError: pass
    else: raise AssertionError("missing Lua capability accepted")
    project["components"][0]["capabilities"] = 65
    for includes in (["../outside.lua"], "main.lua", ["main.lua"] * 9, [None]):
        project["components"][0]["logic"]["includes"] = includes
        try: compile_project(project, root)
        except (ValueError, TypeError): pass
        else: raise AssertionError("invalid Lua includes accepted")
    project["components"][0]["logic"].pop("includes")
    project["components"][0]["logic"]["path"] = "../outside.lua"
    try: compile_project(project, root)
    except ValueError: pass
    else: raise AssertionError("logic escaped project")
print("Lua package compilation, deterministic simulation and invalid-code rejection: passed")
