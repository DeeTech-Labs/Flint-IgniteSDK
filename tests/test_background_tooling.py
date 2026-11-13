"""Background simulator controls and exact metadata-only Companion schemas."""
import hashlib
import json
import math
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

class BackgroundToolingTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "temp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "temp", prefix="background-tooling-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def script(self, step):
        path = self.root / "scenario.json"
        path.write_text(json.dumps({"steps": [step]}), encoding="utf-8")
        return scenario_script(path)

    @staticmethod
    def body(rows=None, **fields):
        policy = dict(count=1, more=0, maximum=2, memory=131072, instructions=16384,
                      native=64, window=100, low=0)
        policy.update(fields)
        header = struct.pack("<BBBIIIIB", *policy.values())
        return header + (BackgroundToolingTests.row() if rows is None else rows)

    @staticmethod
    def row(**fields):
        values = dict(state=2, reason=0, error=0, identity=1, generation=2,
                      used=100, peak=200, reserved=49152, callbacks=4, instructions=128,
                      native=3, wall=50, wakeups=4, rejected=0, coalesced=1,
                      execute=1, autogrant=1, auto=0)
        values.update(fields)
        return (text("test.background-tooling") + text("worker") + text("Worker") +
                struct.pack("<BBHQQIIIQQQQQQQBBB", *values.values()))

    def test_explicit_scenario_controls(self):
        for operation in ("service_start", "service_stop", "service_pause", "service_resume", "service_revoke"):
            self.assertEqual(self.script({"op": operation, "component": "worker"}),
                             operation + " - 776f726b6572 0\n")
        self.assertEqual(self.script({"op": "service_auto", "component": "worker", "enabled": True}),
                         "service_auto - 776f726b6572 1 0\n")
        self.assertEqual(self.script({"op": "expect_service", "component": "worker", "state": 3, "reason": 3}),
                         "expect_service - 776f726b6572 3 3\n")
        self.assertEqual(self.script({"op": "low_energy", "enabled": True}), "low_energy 1\n")

    def test_reject_invalid_scenario_controls(self):
        base = {"op": "service_start", "component": "worker"}
        for changed in ({"component": None}, {"component": "../worker"}, {"component": ""},
                        {"package": 1}, {"package": "bad..id"}, {"expect": True},
                        {"expect": 17}, {"unknown": 1}):
            with self.subTest(changed=changed), self.assertRaises(ValueError): self.script({**base, **changed})
        for step in ({"op": "service_auto", "component": "worker", "enabled": 1},
                     {"op": "expect_service", "component": "worker", "state": 5},
                     {"op": "expect_service", "component": "worker", "state": 0, "reason": 9},
                     {"op": "low_energy", "enabled": math.nan},
                     {"op": "low_energy", "enabled": True, "permission": "app.background.execute"}):
            with self.assertRaises(ValueError): self.script(step)

    def test_exact_services_schema_and_read_only_behavior(self):
        device = FakeCompanion({"services": self.body()})
        report = device.background_services(3, 8)
        self.assertEqual(device.calls, [("services", struct.pack("<HB", 3, 8))])
        row, = report["services"]
        self.assertEqual((row["state"], row["reason"], row["event_coalesced"]), (2, 0, 1))
        self.assertTrue(row["execute_granted"])
        self.assertFalse(row["autostart"])
        self.assertFalse(report["low_energy"])
        self.assertNotIn("state_values", row)
        self.assertNotIn("values", row)
        for args in ((True, 1), (0, False), (-1, 1), (65535, 2), (0, 9), (math.nan, 1)):
            with self.assertRaises(ValueError): device.background_services(*args)

    def test_malformed_response_is_rejected(self):
        malformed = [self.body()[:-1], self.body() + b"extra", self.body(count=9),
                     self.body(more=2), self.body(rows=b"", count=0, more=1),
                     self.body(maximum=5), self.body(memory=0), self.body(window=0),
                     self.body(low=2), self.body(rows=self.row(state=5)),
                     self.body(rows=self.row(reason=9)), self.body(rows=self.row(error=17)),
                     self.body(rows=self.row(execute=2)), self.body(rows=self.row(generation=0)),
                     self.body(rows=self.row() * 2, count=2)]
        for data in malformed:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                FakeCompanion({"services": data}).background_services()

    def test_controls_never_grant_permissions_and_reject_trailing_data(self):
        device = FakeCompanion({"service_control": b""})
        for action in range(7): device.service_control(action, "test.app", "worker")
        self.assertEqual([method for method, _ in device.calls], ["service_control"] * 7)
        self.assertEqual(device.calls[-1][1], b"\6" + text("test.app") + text("worker"))
        for args in ((True, "test.app", "worker"), (7, "test.app", "worker"),
                     (math.inf, "test.app", "worker"), (0, "../app", "worker"), (0, "test.app", None)):
            with self.assertRaises(ValueError): device.service_control(*args)
        with self.assertRaises(ValueError):
            FakeCompanion({"service_control": b"trailing"}).service_control(0, "test.app", "worker")

    def test_old_protocol_and_revision_are_rejected_without_start(self):
        def hello(protocol, revision):
            return struct.pack("<HI", protocol, revision) + text("build") + text("profile") + struct.pack("<Q", 65 | 16384 | 32768)
        for data in (hello(2, REVISION), hello(1, REVISION - 1), hello(1, REVISION) + b"trailing"):
            device = FakeCompanion({"hello": data})
            with self.assertRaises(ValueError): device.hello()
            self.assertEqual([method for method, _ in device.calls], ["hello"])
        device = FakeCompanion({"hello": hello(1, REVISION)})
        device.hello()
        self.assertEqual([method for method, _ in device.calls], ["hello"])

    @unittest.skipUnless(len(TOOLS) == 2, "requires ignite-run and ignite-check executable arguments")
    def test_actual_core_ui_and_service_lifecycle(self):
        runner, validator = map(str, TOOLS)
        (self.root / "ui.lua").write_text("ignite.schedule(0); return {update=function() end}")
        (self.root / "worker.lua").write_text("return {event=function(kind) end}")
        project = {"id": "test.background-tooling", "name": "Background tooling", "publisher": "tests", "version": 1,
          "components": [
            {"id": "main", "type": "app", "app_api": 1, "capabilities": 65,
             "budget": {"internal": 49152, "external": 0, "instructions": 4096},
             "permissions": ["ui.render", "time.timer.create"],
             "logic": {"language": "lua", "path": "ui.lua"},
             "ui": [{"kind": "rect", "w": 280, "h": 240}, {"kind": "text", "text": "UI kept"}]},
            {"id": "worker", "type": "service", "capabilities": 64,
             "budget": {"internal": 49152, "external": 0, "instructions": 4096},
             "permissions": ["app.background.execute", "app.background.autostart"],
             "logic": {"language": "lua", "path": "worker.lua"}}]}
        package = self.root / "mixed.ignit"
        package.write_bytes(compile_project(project, self.root))
        subprocess.run([validator, str(package)], check=True, capture_output=True, timeout=10)
        def control(op, **extra): return {"op": op, "component": "worker", **extra}
        def state(value, **extra): return control("expect_service", state=value, **extra)
        steps = [{"op": "expect_text", "text": "UI kept"}, state(0),
                 control("service_start", expect=5),
                 {"op": "permission", "permission": "app.background.execute", "enabled": True},
                 state(0), control("service_start"), state(2), {"op": "advance", "ms": 0},
                 {"op": "expect_text", "text": "UI kept"}, control("service_pause"), state(3, reason=1),
                 {"op": "low_energy", "enabled": True}, {"op": "low_energy", "enabled": False}, state(3, reason=1),
                 control("service_resume"), state(2), {"op": "low_energy", "enabled": True}, state(3, reason=3),
                 {"op": "low_energy", "enabled": False}, state(2), control("service_auto", enabled=True, expect=5),
                 {"op": "permission", "permission": "app.background.autostart", "enabled": True},
                 control("service_auto", enabled=True), control("service_stop"), state(0),
                 {"op": "reboot"}, {"op": "advance", "ms": 0}, state(2),
                 control("service_auto", enabled=False), control("service_revoke"), state(0, reason=2),
                 control("service_start", expect=5), {"op": "expect_text", "text": "UI kept"}]
        scenario = self.root / "scenario.json"
        scenario.write_text(json.dumps({"steps": steps}))
        result = subprocess.run([sys.executable, str(ROOT / "tools/ignite.py"), "run", str(package),
                    "--runner", runner, "--scenario", str(scenario), "--image", str(self.root / "mixed.ppm")],
                    capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["background_simulation"])
        self.assertTrue(report["simulation_control"])
        self.assertFalse(report["sensor_simulation"])
        self.assertFalse(report["background_low_energy"])
        self.assertNotIn("service_state_values", report)
        # Native receiver independently rejects malformed controls before dispatch.
        for index, script in enumerate(("service_start - 776f726b6572 0 trailing\n",
                "service_auto - 776f726b6572 2 0\n", "expect_service - 776f726b6572 5 255\n", "low_energy nan\n")):
            native = [runner, str(package), "1", "0", str(self.root / "invalid.ppm"), "--scenario",
                      str(self.root / ("store" + str(index))), str(self.root / "factory")]
            rejected = subprocess.run(native, input=script, text=True, capture_output=True, timeout=10)
            self.assertNotEqual(rejected.returncode, 0)
        # Revision failure is checked by the native package/Core path before any execution.
        old = bytearray(package.read_bytes()); struct.pack_into("<I", old, 12, REVISION - 1)
        old[32:64] = bytes(32); old[32:64] = hashlib.sha256(old).digest()
        old_package = self.root / "old.ignit"; old_package.write_bytes(old)
        rejected = subprocess.run([runner, str(old_package), "1", "0", str(self.root / "old.ppm")], capture_output=True, timeout=10)
        self.assertNotEqual(rejected.returncode, 0)
        # A service-only package keeps a factory clock in the same Core; no substitute UI VM.
        clock = {"id": "test.factory-clock", "name": "Clock", "publisher": "tests", "version": 1,
                 "components": [{"id": "main", "type": "watchface", "role": "clock", "capabilities": 65,
                    "app_api": 1, "budget": {"internal": 49152, "external": 0, "instructions": 4096},
                    "permissions": ["ui.render"], "logic": {"language": "lua", "path": "clock.lua"},
                    "ui": [{"kind": "rect", "w": 280, "h": 240}, {"kind": "text", "text": "Factory clock kept"}]}]}
        (self.root / "clock.lua").write_text("return {update=function() end}")
        factory = self.root / "clock-factory"; factory.mkdir()
        (factory / "clock.ignit").write_bytes(compile_project(clock, self.root))
        project["components"] = [project["components"][1]]
        service_only = self.root / "service-only.ignit"; service_only.write_bytes(compile_project(project, self.root))
        tokens = "expect_service - 776f726b6572 0 255\npermission app.background.execute - 1\nservice_start - 776f726b6572 0\nexpect_service - 776f726b6572 2 255\nexpect_text 466163746f727920636c6f636b206b657074\nservice_stop - 776f726b6572 0\n"
        result = subprocess.run([runner, str(service_only), "1", "0", str(self.root / "clock.ppm"), "--scenario",
                                str(self.root / "service-only-store"), str(factory)],
                                input=tokens, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == "__main__": unittest.main()
