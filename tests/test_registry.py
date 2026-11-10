"""Reject ambiguous service specifications before emitting C++ registries."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from generate_registry import validate_services


class RegistryTests(unittest.TestCase):
    def test_valid_services(self):
        validate_services([
            {"id": 1, "name": "hello", "mutates": False},
            {"id": 65535, "name": "last_method", "mutates": True},
        ])

    def test_invalid_services(self):
        first = {"id": 1, "name": "hello", "mutates": False}
        for override in (
            {"id": 1, "name": "other"},
            {"id": 2, "name": "hello"},
            {"id": 0}, {"id": 65536}, {"id": True},
            {"name": "invalid-name"}, {"name": ""},
            {"mutates": "false"},
        ):
            second = {"id": 2, "name": "other", "mutates": False, **override}
            with self.subTest(override=override), self.assertRaises(ValueError):
                validate_services([first, second])


if __name__ == "__main__":
    unittest.main()
