import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "builder", ROOT / "tools/build_package.py"
)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
sys.path.insert(0, str(ROOT / "tools"))
from compile_ignite import scene


class BuilderTests(unittest.TestCase):
    def test_opt_in_text_boxes(self):
        background = {"kind": "rect", "w": 280, "h": 240}
        for mode, flags in (("clip", 8), ("wrap", 10), ("ellipsis", 12)):
            data = scene([background, {"kind":"text", "text":"Датчики unavailable", "x":10,
                          "y":20,"w":200,"h":60,"overflow":mode,"center":True}])
            self.assertEqual(struct.unpack_from(">H",data,10+18+16)[0], flags|1)
        free = scene([background,{"kind":"text","text":"free","center":True}])
        self.assertEqual(struct.unpack_from(">H",free,10+18+16)[0],1)
        for mode, width, height in (("wrap",0,50),("wrap",50,0),("unknown",50,50)):
            with self.assertRaises(ValueError):
                scene([background,{"kind":"text","w":width,"h":height,"overflow":mode}])

    def test_size_limits(self):
        self.assertTrue(builder.build([(1, "manifest", b"x" * 4096)]))
        with self.assertRaisesRegex(ValueError, "manifest"):
            builder.build([(1, "manifest", b"x" * 4097)])
        # Include header and both index entries in the package limit.
        resource_size = 1024 * 1024 - 64 - 2 * 24 - 2
        sections = [(1, "manifest", b"{}"), (2, "resource", b"x" * resource_size)]
        self.assertEqual(len(builder.build(sections)), 1024 * 1024)
        with self.assertRaisesRegex(ValueError, "package"):
            builder.build([sections[0], (2, "resource", b"x" * (resource_size + 1))])

    def test_duplicate_and_empty(self):
        for sections in (
            [],
            [(1, "manifest", b"")],
            [(1, "manifest", b"x"), (1, "ui", b"y")],
            [(1, "ui", b"x")],
        ):
            with self.subTest(sections=sections), self.assertRaises(ValueError):
                builder.build(sections)

    def test_independent_digest(self):
        for length in (1, 55, 56, 63, 64, 65, 255, 256, 257, 1024):
            data = builder.build(
                [(1, "manifest", b"{}"), (2, "resource", b"x" * length)]
            )
            self.assertEqual(
                data[32:64], hashlib.sha256(data[:32] + bytes(32) + data[64:]).digest()
            )
            self.assertEqual(struct.unpack_from("<I", data, 16)[0], len(data))


if __name__ == "__main__":
    if len(sys.argv) > 1:
        executable = sys.argv.pop(1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.ignit"
            for length in (3, 55, 56, 63, 64, 65, 256, 257):
                path.write_bytes(
                    builder.build(
                        [
                            (1, "manifest", b"{}"),
                            (2, "resource", b"abc" + b"x" * (length - 3)),
                        ]
                    )
                )
                subprocess.run([executable, str(path)], check=True)
    unittest.main()
