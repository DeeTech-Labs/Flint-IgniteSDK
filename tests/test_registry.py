"""Reject ambiguous service specifications before emitting C++ registries."""
import sys
import copy
import json
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from generate_registry import validate_services, sensor_registry


class RegistryTests(unittest.TestCase):
    def sensor_data(self):
        root = Path(__file__).resolve().parents[1]
        data = json.loads((root / "spec/sensors.json").read_text())
        permissions = json.loads((root / "spec/permissions.json").read_text())
        names = {name for key, group in permissions.items() if key != "version" for name in group}
        return data, names

    def test_sensor_generated_contract(self):
        data, permissions = self.sensor_data()
        generated = sensor_registry(data, permissions)
        self.assertEqual(generated, sensor_registry(copy.deepcopy(data), permissions))
        for channel in data["channels"]:
            self.assertIn(channel["name"] + " = " + str(channel["id"]), generated)
            for field in ("name", "unit", "permission"):
                self.assertIn(json.dumps(channel[field]), generated)
        for function in ("name", "unit", "permission"):
            self.assertIn(function + "(Channel c)", generated)

    def test_invalid_sensor_channels(self):
        data, permissions = self.sensor_data()
        for fields in ({"id": True}, {"id": 1}, {"name": "pressure"},
                {"name": "invalid-name"}, {"name": "температура"},
                {"permission": "unknown.permission"}, {"unit": ""}, {"unit": "bad\"unit"}):
            mutated = copy.deepcopy(data); mutated["channels"][0].update(fields)
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                sensor_registry(mutated, permissions)
        for mutation in ({**data, "version": 2}, {**data, "channels": data["channels"][:-1]}):
            with self.assertRaises(ValueError): sensor_registry(mutation, permissions)

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
