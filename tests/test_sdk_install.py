"""Prove that a relocated SDK works without its source checkout or Flare."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

build = Path(sys.argv[1]).resolve()
cmake = sys.argv[2]
configuration = sys.argv[3] if len(sys.argv) > 3 else "Debug"


def run(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="ignite installed ") as directory:
    root = Path(directory)
    prefix = root / "sdk"
    run(cmake, "--install", build, "--prefix", prefix, "--config", configuration)
    sdk = prefix / "bin/ignite"
    run(sys.executable, sdk, "init", root / "clock", "--id", "demo.clock", "--publisher", "demo")
    report = json.loads(run(sys.executable, sdk, "build", root / "clock/project.json",
                            root / "clock.ignit").stdout)
    assert report["runtime_compatible"] and not report["device_fit_verified"]
    run(sys.executable, sdk, "check", root / "clock.ignit")
    consumer = root / "consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.21)
project(SDKConsumer LANGUAGES CXX)
find_package(FlintPlatform 1.0 EXACT CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Flint::Platform)
''')
    (consumer / "main.cpp").write_text('''#include <flint/runtime.h>
int main() { flint::Runtime runtime; return int(runtime.stop()); }
''')
    run(cmake, "-S", consumer, "-B", root / "build", f"-DCMAKE_PREFIX_PATH={prefix}")
    run(cmake, "--build", root / "build", "--config", configuration)
