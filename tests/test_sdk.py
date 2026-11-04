"""End-to-end SDK tests against the production portable validator."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(ROOT / "tools"))
from build_package import build
from compile_ignite import compile_project


class SDKTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ignite sdk ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "project.json"
        self.output = self.root / "app.ignit"
        self.project = json.loads((ROOT / "examples/pc-monitor/project.json").read_text())

    def cli(self, *args, success=True):
        result = subprocess.run([sys.executable, str(ROOT / "tools/ignite.py"), *map(str, args)],
                                capture_output=True, text=True, cwd=self.root)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def compile(self, success=True):
        self.source.write_text(json.dumps(self.project))
        return self.cli("build", self.source, self.output, "--validator", VALIDATOR, success=success)

    def test_init_build_check(self):
        directory = self.root / "new project"
        self.cli("init", directory, "--id", "demo.clock", "--publisher", "demo")
        result = self.cli("build", directory / "project.json", self.output, "--validator", VALIDATOR)
        report = json.loads(result.stdout)
        self.assertEqual(report["id"], "demo.clock")
        self.assertFalse(report["device_fit_verified"])
        self.assertFalse(report["publisher_verified"])
        checked = self.cli("check", self.output, "--validator", VALIDATOR)
        self.assertEqual(report, json.loads(checked.stdout))
        self.cli("init", directory, "--id", "demo.clock", "--publisher", "demo", success=False)

    def test_corrupt_package(self):
        self.compile()
        data = bytearray(self.output.read_bytes())
        data[-1] ^= 1
        self.output.write_bytes(data)
        result = self.cli("check", self.output, "--validator", VALIDATOR, success=False)
        self.assertIn("integrity_failed", result.stderr)

    def test_failed_build_preserves_output(self):
        self.compile()
        previous = self.output.read_bytes()
        self.project["permissions"] = ["unknown.permission"]
        self.compile(success=False)
        self.assertEqual(previous, self.output.read_bytes())

    def test_missing_resource_permission(self):
        self.project["components"][0]["ui"].append(
            {"kind": "image", "resource": 100, "w": 1, "h": 1})
        self.assertIn("permission_denied", self.compile(success=False).stderr)

    def test_missing_resource(self):
        self.project["permissions"].append("ui.resources")
        self.project["components"][0]["ui"].append(
            {"kind": "image", "resource": 100, "w": 1, "h": 1})
        self.assertIn("invalid_package", self.compile(success=False).stderr)

    def test_duplicate_component_ids(self):
        self.project["components"].append(dict(self.project["components"][0]))
        self.compile(success=False)

    def test_budget_rejected(self):
        self.project["budget"] = {"internal": 1}
        self.assertIn("quota_exceeded", self.compile(success=False).stderr)

    def test_code_and_styles_not_silently_discarded(self):
        for field in ("logic", "javascript", "lua", "code", "script", "styles", "css"):
            with self.subTest(field=field):
                self.project[field] = "source"
                self.compile(success=False)
                del self.project[field]
                self.project["components"][0][field] = "source"
                self.compile(success=False)
                del self.project["components"][0][field]

    def test_source_not_overwritten(self):
        self.source.write_text(json.dumps(self.project))
        previous = self.source.read_bytes()
        self.cli("build", self.source, self.source, "--validator", VALIDATOR, success=False)
        self.assertEqual(previous, self.source.read_bytes())

    def test_missing_validator(self):
        self.source.write_text(json.dumps(self.project))
        self.cli("build", self.source, self.output, "--validator", self.root / "missing", success=False)
        self.assertFalse(self.output.exists())

    def test_all_factory_projects(self):
        for source in (ROOT / "system").glob("*.json"):
            with self.subTest(source=source.name):
                self.cli("build", source, self.output, "--validator", VALIDATOR)


if __name__ == "__main__":
    unittest.main()
