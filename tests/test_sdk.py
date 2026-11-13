"""End-to-end SDK tests against the production portable validator."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = Path(sys.argv.pop(1)).resolve()
WITHOUT_LUA = "--without-lua" in sys.argv
if WITHOUT_LUA:
    sys.argv.remove("--without-lua")
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

    def cli(self, *args, success=True, env=None):
        result = subprocess.run([sys.executable, str(ROOT / "tools/ignite.py"), *map(str, args)],
                                capture_output=True, text=True, encoding="utf-8", cwd=self.root, env=env)
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

    def test_unicode_report_under_non_utf8_locale(self):
        self.project["name"] = "Русский циферблат"
        self.source.write_text(json.dumps(self.project), encoding="utf-8")
        environment = dict(os.environ, PYTHONIOENCODING="cp1252", PYTHONUTF8="0",
                           PYTHONCOERCECLOCALE="0", LC_ALL="C")
        result = self.cli("build", self.source, self.output, "--validator", VALIDATOR,
                          env=environment)
        self.assertEqual(json.loads(result.stdout)["name"], self.project["name"])

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
        self.project["components"][0]["permissions"] = ["unknown.permission"]
        self.compile(success=False)
        self.assertEqual(previous, self.output.read_bytes())

    def test_missing_resource_permission(self):
        self.project["components"][0]["ui"].append(
            {"kind": "image", "resource": 100, "w": 1, "h": 1})
        self.assertIn("permission_denied", self.compile(success=False).stderr)

    def test_missing_resource(self):
        self.project["components"][0]["permissions"].append("ui.resources")
        self.project["components"][0]["ui"].append(
            {"kind": "image", "resource": 100, "w": 1, "h": 1})
        self.assertIn("invalid_package", self.compile(success=False).stderr)

    def test_duplicate_component_ids(self):
        self.project["components"].append(dict(self.project["components"][0]))
        self.compile(success=False)

    def test_budget_rejected(self):
        self.project["components"][0]["budget"] = {"internal": 1, "external": 0, "instructions": 4096}
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

    def test_component_manifest_service_envelope_and_local_declarations(self):
        (self.root / "main.lua").write_text("return {update=function() end}")
        (self.root / "service.lua").write_text("return {event=function() end}")
        self.project = {
            "id": "test.components", "name": "Components", "publisher": "tests", "version": 1,
            "components": [
                {"id": "main", "type": "app", "app_api": 1, "capabilities": 65,
                 "budget": {"internal": 49152, "external": 0, "instructions": 4096},
                 "permissions": ["ui.render"], "logic": {"language": "lua", "path": "main.lua"},
                 "ui": [{"kind": "rect", "w": 280, "h": 240}]},
                {"id": "worker", "type": "service", "capabilities": 64,
                 "budget": {"internal": 98304, "external": 1024, "instructions": 12000},
                 "permissions": ["storage.private", "app.background.execute"],
                 "logic": {"language": "lua", "path": "service.lua"}},
            ]}
        if WITHOUT_LUA:
            self.compile(success=False)
            return
        report = json.loads(self.compile().stdout)
        self.assertEqual(report["declared_budget"], {"internal_bytes": 98304, "external_bytes": 1024, "instructions": 12000})
        self.assertTrue(report["required_capabilities"] & 16384)
        # The service never declares ui.render; union grants must not become its local rights.
        self.assertNotIn("ui.render", self.project["components"][1]["permissions"])
        for root_field, value in (("budget", {}), ("permissions", []), ("capabilities", 1), ("logic", {}), ("app_api", 1)):
            self.project[root_field] = value
            self.compile(success=False)
            del self.project[root_field]
        service = self.project["components"][1]
        service["permissions"].append("ui.render")
        self.compile(success=False)
        service["permissions"].pop()
        service["permissions"].remove("app.background.execute")
        self.compile(success=False)
        service["permissions"].append("app.background.execute")
        service["logic"]["section"] = 20
        self.project["components"][0]["logic"]["section"] = 20
        self.compile(success=False)
        del self.project["components"][0]["logic"]["section"]
        del service["logic"]["section"]
        self.project["components"].append(dict(service, id="second"))
        self.compile(success=False)

    def test_all_factory_projects(self):
        for source in (ROOT / "system").glob("*.json"):
            with self.subTest(source=source.name):
                requires_lua = any("logic" in c for c in json.loads(source.read_text(encoding="utf-8"))["components"])
                if WITHOUT_LUA and requires_lua:
                    result = self.cli("build", source, self.output, "--validator", VALIDATOR, success=False)
                    self.assertIn("not_supported", result.stderr)
                else:
                    self.cli("build", source, self.output, "--validator", VALIDATOR)


if __name__ == "__main__":
    unittest.main()
