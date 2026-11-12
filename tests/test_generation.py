"""Regression coverage for build-local, deterministic SDK generation."""
import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def snapshot(root):
    return {str(p.relative_to(root)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
            for p in root.rglob("*") if p.is_file()
            and "build" not in p.relative_to(root).parts
            and "__pycache__" not in p.parts}


class GenerationTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "temp").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="ignite generation ", dir=ROOT / "temp")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.sdk = self.work / "sdk"
        for name in ["tools", "cmake", "spec", "system", "examples", "include", "factory"]:
            shutil.copytree(ROOT / name, self.sdk / name,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    def run_tool(self, name, *args, success=True):
        result = subprocess.run([sys.executable, "-B", str(self.sdk / "tools" / name),
                                 *map(str, args)], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is not installed")
    def test_cmake_relative_output_belongs_to_build_directory(self):
        before = snapshot(self.sdk)
        consumer = self.work / "consumer"
        consumer.mkdir()
        (consumer / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(GenerationFixture NONE)\n'
            f'include("{self.sdk.as_posix()}/cmake/FlintGenerate.cmake")\n'
            'flint_generate_sdk(OUTPUT_DIR generated EXAMPLES)\n', encoding="utf-8")
        build = self.work / "consumer-build"
        result = subprocess.run(["cmake", "-S", str(consumer), "-B", str(build),
                                 f"-DPython3_EXECUTABLE={sys.executable}"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((build / "generated/include/flint/registry.h").is_file())
        self.assertTrue((build / "generated/factory/boot.ignit").is_file())
        self.assertFalse((consumer / "generated").exists())
        self.assertEqual(before, snapshot(self.sdk))

    def test_defaults_leave_sources_untouched(self):
        before = snapshot(self.sdk)
        self.run_tool("generate_registry.py")
        self.run_tool("build_system.py")
        generated = self.sdk / "build/generated"
        self.assertTrue((generated / "include/flint/services.h").is_file())
        self.assertTrue((generated / "factory/boot.ignit").is_file())
        self.assertTrue((generated / "examples/counter/sample.ignit").is_file())
        self.assertEqual(before, snapshot(self.sdk))

    def test_icon_authoring_defaults_to_output_directory(self):
        before = snapshot(self.sdk)
        self.run_tool("build_system_icons.py")
        output = self.sdk / "build/generated/system-icons"
        self.assertEqual(len(list(output.glob("*.png"))), 4)
        self.assertEqual(before, snapshot(self.sdk))

    def test_font_authoring_defaults_to_output_directory(self):
        before = snapshot(self.sdk)
        fonts = self.work / "fonts"
        fonts.mkdir()
        # One licensed test font is sufficient to verify output routing for all roles.
        for name in ["Lato-Regular.ttf", "Lato-Bold.ttf", "Lato-Light.ttf"]:
            shutil.copyfile(ROOT / "tests/assets/Lato-Regular.ttf", fonts / name)
        self.run_tool("build_builtin_fonts.py", "--font-dir", fonts)
        output = self.sdk / "build/generated/fonts"
        self.assertEqual(len(list(output.glob("*.inc"))), 4)
        self.assertEqual(before, snapshot(self.sdk))
        times = {p.name: p.stat().st_mtime_ns for p in output.iterdir()}
        self.run_tool("build_builtin_fonts.py", "--font-dir", fonts)
        self.assertEqual(times, {p.name: p.stat().st_mtime_ns for p in output.iterdir()})

    def test_explicit_outputs_are_isolated_and_idempotent(self):
        before = snapshot(self.sdk)
        output = self.work / "out"
        def generate():
            self.run_tool("generate_registry.py", "--output", output / "include")
            self.run_tool("build_system.py", "--output", output / "factory",
                          "--examples-output", output / "examples")
        generate()
        times = {str(p.relative_to(output)): p.stat().st_mtime_ns
                 for p in output.rglob("*") if p.is_file()}
        generate()
        self.assertEqual(times, {str(p.relative_to(output)): p.stat().st_mtime_ns
                                for p in output.rglob("*") if p.is_file()})
        self.assertEqual(before, snapshot(self.sdk))
        self.assertFalse((self.sdk / "build").exists())

    def test_removed_examples_prune_only_generated_samples(self):
        output = self.work / "out"
        def generate():
            self.run_tool("build_system.py", "--output", output / "factory",
                          "--examples-output", output / "examples")
        generate()
        counter = output / "examples/counter"
        self.assertTrue((counter / "sample.ignit").exists())
        (counter / "notes.txt").write_bytes(b"unrelated local work")
        removed = output / "examples/removed"
        removed.mkdir()
        (removed / "sample.ignit").write_bytes(b"old generated output")
        (self.sdk / "examples/counter/project.json").unlink()
        generate()
        self.assertFalse((counter / "sample.ignit").exists())
        self.assertEqual((counter / "notes.txt").read_bytes(), b"unrelated local work")
        self.assertFalse(removed.exists())

    def test_skip_examples_and_failed_compile_preserve_previous_output(self):
        output = self.work / "factory"
        self.run_tool("build_system.py", "--output", output, "--skip-examples")
        self.assertFalse((self.sdk / "build").exists())
        (output / "previous.ignit").write_bytes(b"previous successful output")
        (output / "incoming.part").write_bytes(b"unrelated local work")
        before = snapshot(output)
        (self.sdk / "system/zz-invalid.json").write_text("{}", encoding="utf-8")
        self.run_tool("build_system.py", "--output", output, "--skip-examples", success=False)
        self.assertEqual(before, snapshot(output))
        (self.sdk / "system/zz-invalid.json").unlink()
        self.run_tool("build_system.py", "--output", output, "--skip-examples")
        self.assertFalse((output / "previous.ignit").exists())
        self.assertEqual((output / "incoming.part").read_bytes(), b"unrelated local work")


if __name__ == "__main__":
    unittest.main()
