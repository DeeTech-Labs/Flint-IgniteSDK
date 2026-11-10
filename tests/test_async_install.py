import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_package import build, TYPES
from compile_ignite import compile_project

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    counter = ROOT / "examples/counter/sample.ignit"
    project = json.loads((ROOT / "examples/pc-monitor/project.json").read_text(encoding="utf-8"))
    project["id"] = "test.bulk"
    data = compile_project(project, ROOT / "examples/pc-monitor")
    names = {value: key for key, value in TYPES.items()}
    def padded(data):
        sections = []
        for index in range(struct.unpack_from("<I", data, 20)[0]):
            ident, kind, _, offset, size, _, _ = struct.unpack_from("<IHHIIII", data, 64 + index * 24)
            sections.append((ident, names[kind], data[offset:offset + size]))
        sections.append((10000, "resource", bytes(range(256)) * 2800))
        return build(sections)
    bulk = root / "bulk.ignit"
    bulk.write_bytes(padded(data))
    updated = json.loads((ROOT / "examples/counter/project.json").read_text(encoding="utf-8"))
    updated["version"] = 2
    counter_update = root / "counter-update.ignit"
    counter_update.write_bytes(padded(compile_project(updated, ROOT / "examples/counter")))
    subprocess.run([sys.argv[1], str(root / "store"), str(ROOT / "factory"),
                    str(counter), str(bulk), str(counter_update)], check=True, timeout=30)
