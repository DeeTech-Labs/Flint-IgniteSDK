"""Sensor JSON controls and exact Companion schemas, without opening a serial port."""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ignite import scenario_script
from companion import Companion, text
from compile_ignite import compile_project
from build_package import REVISION

TOOLS = sys.argv[1:]
sys.argv[1:] = []

class FakeCompanion(Companion):
    def __init__(self, replies): self.replies, self.calls = replies, []
    def request(self, method, data=b"", timeout=5):
        self.calls.append((method, data)); return self.replies[method]

class SensorToolingTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "temp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "temp", prefix="sensor-tooling-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def script(self, step):
        path = self.root / "scenario.json"; path.write_text(json.dumps({"steps": [step]}))
        return scenario_script(path)

    def test_controls(self):
        self.assertEqual(self.script({"op": "sensor_value", "source": 1, "channel": "temperature", "x": 23.5}), "sensor_value 1 0 23.5 0 0\n")
        self.assertEqual(self.script({"op": "sensor_state", "source": 1, "status": 5}), "sensor_state 1 5 7\n")
        self.assertEqual(self.script({"op": "sensor_reset", "source": 1}), "sensor_reset 1\n")

    def test_reject_invalid_controls(self):
        base = {"op": "sensor_value", "source": 1, "channel": "temperature", "x": 1}
        for fields in ({"x": math.nan}, {"x": math.inf}, {"x": -math.inf}, {"x": 1e13}, {"x": 10**1000},
                {"x": True}, {"source": 0}, {"source": 65536}, {"channel": True},
                {"channel": 6}, {"channel": "unknown"}, {"y": 1}, {"unknown": 1}):
            with self.subTest(fields=fields), self.assertRaises(ValueError): self.script({**base, **fields})
        for step in ({"op": "sensor_state", "source": 1, "status": 6},
                {"op": "sensor_state", "source": 1, "status": 2, "reason": 7},
                {"op": "sensor_state", "source": 1, "status": 4, "reason": 17},
                {"op": "sensor_reset", "source": False}):
            with self.assertRaises(ValueError): self.script(step)

    @staticmethod
    def catalog_row():
        return struct.pack("<HBIIIIBBBBBHQQQQ", 1, 7, 100, 1000, 60000, 5000,
            0, 1, 1, 0, 2, 0, 3, 5, 1000, 10)

    def test_catalog_schema(self):
        device = FakeCompanion({"sensor_catalog": b"\1" + self.catalog_row()})
        source, = device.sensor_catalog()
        self.assertEqual((source["source_id"], source["channels"], source["sample_id"]), (1, 7, 5))
        self.assertTrue(source["simulation"])
        self.assertNotIn("values", source)
        for data in (b"\x11", b"\1" + self.catalog_row()[:-1], b"\0extra",
                b"\2" + self.catalog_row() * 2):
            with self.assertRaises(ValueError): FakeCompanion({"sensor_catalog": data}).sensor_catalog()

    def test_diagnostic_schema(self):
        header = struct.pack("<HBQQQQBHIB", 1, 1, 2, 3, 1000, 10, 2, 0, 4, 1)
        result = FakeCompanion({"sensor_diagnostic": header + struct.pack("<ddd", 23.5, 0, 0)}).sensor_diagnostic_read()
        self.assertEqual(result["values"], {"temperature": {"x": 23.5, "y": 0.0, "z": 0.0}})
        self.assertEqual(result["drops"], 4)
        pending = struct.pack("<HBQQQQBHIB", 1, 0, 2, 3, 1000, 10, 1, 7, 0, 1)
        self.assertFalse(FakeCompanion({"sensor_diagnostic": pending}).sensor_diagnostic_read()["values"])
        for data in (header, header + struct.pack("<ddd", math.nan, 0, 0),
                header + struct.pack("<ddd", 23, 1, 0), pending + b"extra"):
            with self.assertRaises(ValueError): FakeCompanion({"sensor_diagnostic": data}).sensor_diagnostic_read()

    def test_explicit_leases_and_time(self):
        device = FakeCompanion({"sensor_diagnostic": b"", "time_status": struct.pack("<QBH", 1793740000, 3, 11)})
        device.sensor_diagnostic_start(1, 1, 200, 30000)
        self.assertEqual(device.calls[-1][1], struct.pack("<BHBII", 1, 1, 1, 200, 30000))
        device.sensor_diagnostic_stop(); self.assertEqual(device.calls[-1], ("sensor_diagnostic", b"\0"))
        self.assertEqual(device.time_status(), {"epoch": 1793740000, "state": 3, "error": 11})
        for args in ((0, 1, 200, 1000), (1, 64, 200, 1000), (1, 1, 199, 1000), (1, 1, 200, 30001)):
            with self.assertRaises(ValueError): device.sensor_diagnostic_start(*args)
        for response in (b"", struct.pack("<QBH", 0, 4, 0), struct.pack("<QBH", 0, 1, 0) + b"x"):
            with self.assertRaises(ValueError): FakeCompanion({"time_status": response}).time_status()

    def test_hello_does_not_enable_sensors(self):
        hello = struct.pack("<HI", 1, REVISION) + text("build") + text("profile") + struct.pack("<Q", 65 | 8192)
        device = FakeCompanion({"hello": hello}); device.hello()
        self.assertEqual([method for method, _ in device.calls], ["hello"])

    @unittest.skipUnless(len(TOOLS) == 2, "requires ignite-run and ignite-check executable arguments")
    def test_actual_core_scenario(self):
        runner, validator = map(str, TOOLS)
        project = {
            'id': 'test.sensor-tooling',
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
                            'text': 't=${app.t};g=${app.g};e=${app.e};sim=${app.sim};'
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
                        'sensors.temperature'
                    ]
                }
            ]
        }
        (self.root / "main.lua").write_text('''ignite.schedule(0)
return {update=function() end,event=function(kind,v)
 if v==1 then local _,e=ignite.sensors.subscribe(1,1,100); ignite.value('e',e)
 else local s,e=ignite.sensors.read(1,1,false); ignite.value('e',e) end
end,sensor=function(h,s,e)
 ignite.value('e',e); ignite.value('g',s.generation)
 if s.simulation then ignite.value('sim',1) end
 if e==0 and not s.values.pressure then
  if s.values.temperature.x==23.5 then ignite.value('t',235)
  elseif s.values.temperature.x==31.5 then ignite.value('t',315) end
 end
end}''')
        package = self.root / "app.ignit"; package.write_bytes(compile_project(project, self.root))
        subprocess.run([validator, str(package)], check=True, capture_output=True, timeout=10)
        steps = [{"op": "permission", "permission": "sensors.temperature", "enabled": True},
            {"op": "input", "kind": "press", "value": 1}, {"op": "advance", "ms": 0},
            {"op": "expect_text", "text": "t=235;g=1;e=0;sim=1;"},
            {"op": "sensor_value", "source": 1, "channel": "temperature", "x": 31.5},
            {"op": "advance", "ms": 100}, {"op": "expect_text", "text": "t=315;"},
            {"op": "sensor_state", "source": 1, "status": 4, "reason": 11},
            {"op": "advance", "ms": 100}, {"op": "expect_text", "text": "e=7;"},
            {"op": "sensor_state", "source": 1, "status": 2}, {"op": "sensor_reset", "source": 1},
            {"op": "advance", "ms": 100}, {"op": "expect_text", "text": "g=2;e=0;"}]
        path = self.root / "scenario.json"; path.write_text(json.dumps({"steps": steps}))
        command = [sys.executable, str(ROOT / "tools/ignite.py"), "run", str(package), "--scenario", str(path),
            "--runner", runner, "--image", str(self.root / "sensor.ppm")]
        reports = [subprocess.run(command, check=True, capture_output=True, text=True, timeout=15).stdout for _ in range(2)]
        self.assertEqual(reports[0], reports[1])
        self.assertTrue(json.loads(reports[0])["sensor_simulation"])
        # The native token receiver validates independently of the JSON front end.
        for index, tokens in enumerate(("sensor_value 1 0 nan 0 0\n", "sensor_value 1 0 1e13 0 0\n",
                "sensor_state 1 6 0\n", "sensor_reset 99\n", "sensor_reset 1 trailing\n")):
            native = [runner, str(package), "1", "0", str(self.root / "bad.ppm"), "--scenario",
                str(self.root / ("invalid-store-" + str(index))), str(self.root / "factory")]
            result = subprocess.run(native, input=tokens, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
        # Ordinary manifests do not implicitly acquire a virtual hardware catalog.
        project["components"][0].pop("sensor_api")
        (self.root / "main.lua").write_text("return {update=function() end}")
        plain = self.root / "plain.ignit"; plain.write_bytes(compile_project(project, self.root))
        plain_command = [sys.executable, str(ROOT / "tools/ignite.py"), "run", str(plain), "--frames", "1",
            "--press-every", "0", "--runner", runner, "--image", str(self.root / "plain.ppm")]
        result = subprocess.run(plain_command, check=True, capture_output=True, text=True, timeout=10)
        self.assertFalse(json.loads(result.stdout)["sensor_simulation"])

if __name__ == "__main__": unittest.main()
