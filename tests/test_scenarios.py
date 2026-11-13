import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import compile_project
from ignite import scenario_script
RUN, CHECK = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    project_path = ROOT / "examples/counter/project.json"
    project = json.loads(project_path.read_text())
    package = root / "counter.ignit"
    data = compile_project(project, project_path.parent)
    assert data == compile_project(project, project_path.parent)
    package.write_bytes(data)
    subprocess.run([CHECK, str(package)], check=True, capture_output=True, timeout=5)
    def run(scenario, storage=None, success=True):
        script = root / "scenario.json"
        script.write_text(json.dumps(scenario))
        args = [sys.executable, str(ROOT / "tools/ignite.py"), "run", str(package),
            "--scenario", str(script), "--runner", RUN, "--image", str(root / "counter.ppm")]
        if storage: args += ["--storage", str(storage)]
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return result
    scenario = json.loads((ROOT / "examples/counter/scenario.json").read_text())
    reports = [json.loads(run(scenario).stdout) for _ in range(2)]
    assert reports[0] == reports[1]
    run(scenario, root / "store")
    run({"steps": [{"op": "expect_text", "text": "Count: 2"}]}, root / "store")
    run({"steps": [{"op": "expect_text", "text": "Count: 999"}]}, success=False)
    run({"steps": [{"op": "suspend"}, {"op": "input", "kind": "press", "expect": 13},
        {"op": "resume"}]})
    for invalid in ({"steps": []}, {"steps": [{"op": "advance", "ms": True}]},
        {"steps": [{"op": "advance", "ms": 86400001}]},
        {"steps": [{"op": "input", "kind": "injected"}]},
        {"steps": [{"op": "reboot", "unknown": 1}]},
        {"steps": [{"op": "expect_text", "text": "x" * 193}]}):
        path = root / "invalid.json"; path.write_text(json.dumps(invalid))
        try: scenario_script(path)
        except ValueError: pass
        else: raise AssertionError("malformed scenario accepted")
    for field, value in (("app_api", True), ("app_api", 2)):
        invalid = copy.deepcopy(project); invalid["components"][0][field] = value
        try: compile_project(invalid, project_path.parent)
        except ValueError: pass
        else: raise AssertionError("invalid app API version accepted")
    for name in ("../../escape", "bad name", "indicator"):
        invalid = copy.deepcopy(project); invalid["components"][0]["ui"][2]["id"] = name
        try: compile_project(invalid, project_path.parent)
        except ValueError: pass
        else: raise AssertionError("invalid or duplicate node ID accepted")
print("Core scenarios, persistent counter, restart, reboot and schema validation: passed")
