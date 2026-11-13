#!/usr/bin/env python3
"""Independent Ignite SDK CLI. Uses the shared native validator, never a second parser."""

import argparse
import json
import math
import os
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

from compile_ignite import compile_project, identifier


def validate(path, executable):
    result = subprocess.run(
        [str(executable), str(path.resolve())], capture_output=True, text=True, encoding="utf-8"
    )
    if result.returncode:
        raise ValueError(result.stderr.strip() or "native validator failed")
    return json.loads(result.stdout)


def publish_file(destination, data):
    # Same-directory replacement preserves the previous successful build on any failure.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def scenario_script(path):
    if path.stat().st_size > 256 * 1024:
        raise ValueError("scenario exceeds 256 KiB")
    scenario = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(scenario, dict) or set(scenario) != {"steps"}
            or not isinstance(scenario["steps"], list) or not 1 <= len(scenario["steps"]) <= 4096):
        raise ValueError("scenario requires 1..4096 steps")
    lines = []
    def integer(value, low, high):
        if type(value) is not int or not low <= value <= high:
            raise ValueError("scenario integer out of range")
        return str(value)
    def encoded(value):
        if not isinstance(value, str) or len(value.encode("utf-8")) > 192 or "\0" in value:
            raise ValueError("invalid scenario text")
        return value.encode("utf-8").hex() or "-"
    for step in scenario["steps"]:
        if not isinstance(step, dict) or "op" not in step:
            raise ValueError("scenario step requires op")
        op = step["op"]
        fields = {"op"}
        if op == "advance":
            fields |= {"ms"}; line = "advance " + integer(step.get("ms"), 0, 86400000)
        elif op == "input":
            fields |= {"kind", "value", "expect"}
            kind = step.get("kind")
            if kind not in ("press", "release", "hold", "rotate", "focus_lost", "focus_gained"):
                raise ValueError("invalid input kind")
            line = "input " + kind + " " + integer(step.get("value", 0), -2147483648, 2147483647) + " " + integer(step.get("expect", 0), 0, 15)
        elif op in ("suspend", "resume", "restart", "reboot"):
            line = op
        elif op == "expect_text":
            fields |= {"text"}; line = op + " " + encoded(step.get("text"))
        elif op == "expect_state":
            fields |= {"state"}
            if step.get("state") not in ("active", "suspended", "empty"):
                raise ValueError("invalid expected state")
            line = op + " " + step["state"]
        elif op == "expect_error":
            fields |= {"error"}; line = op + " " + integer(step.get("error"), 0, 15)
        elif op == "permission":
            fields |= {"permission", "scope", "enabled"}
            name = identifier(step.get("permission", ""))
            if type(step.get("enabled")) is not bool:
                raise ValueError("permission enabled must be boolean")
            line = op + " " + name + " " + encoded(step.get("scope", "")) + " " + str(int(step["enabled"]))
        elif op in ("service_start", "service_stop", "service_pause", "service_resume", "service_auto", "service_revoke", "expect_service"):
            fields |= {"package", "component"}
            package = step.get("package", "")
            if type(package) is not str: raise ValueError("invalid service package")
            if package: identifier(package)
            component = step.get("component", "")
            if type(component) is not str: raise ValueError("invalid service component")
            component = identifier(component)
            line = op + " " + encoded(package) + " " + encoded(component)
            if op == "expect_service":
                fields |= {"state", "reason"}
                line += " " + integer(step.get("state"), 0, 4) + " " + integer(step.get("reason", 255), 0, 255)
                if step.get("reason", 255) not in (*range(9), 255): raise ValueError("invalid service reason")
            else:
                fields |= {"expect"}
                if op == "service_auto":
                    fields |= {"enabled"}
                    if type(step.get("enabled")) is not bool: raise ValueError("service autostart enabled must be boolean")
                    line += " " + str(int(step["enabled"]))
                line += " " + integer(step.get("expect", 0), 0, 16)
        elif op == "low_energy":
            fields |= {"enabled"}
            if type(step.get("enabled")) is not bool: raise ValueError("low-energy simulation requires boolean")
            line = op + " " + str(int(step["enabled"]))
        elif op == "sensor_value":
            fields |= {"source", "channel", "x", "y", "z"}
            channels = ("temperature", "pressure", "humidity", "ambient_light", "acceleration", "angular_velocity")
            channel = step.get("channel")
            if isinstance(channel, str):
                if channel not in channels: raise ValueError("invalid sensor channel")
                channel = channels.index(channel)
            channel_text = integer(channel, 0, 5)
            components = [step.get("x"), step.get("y", 0), step.get("z", 0)]
            for component in components:
                if type(component) not in (int, float) or abs(component) > 1e12 or not math.isfinite(component):
                    raise ValueError("sensor components must be finite and within +/-1e12")
            if channel < 4 and (components[1] or components[2]):
                raise ValueError("scalar sensor cannot have y/z components")
            line = op + " " + integer(step.get("source"), 1, 65535) + " " + channel_text + " " + " ".join(format(v, ".17g") for v in components)
        elif op == "sensor_state":
            fields |= {"source", "status", "reason"}
            status = integer(step.get("status"), 0, 5)
            reason = integer(step.get("reason", 0 if status == "2" else 7), 0, 16)
            if status == "2" and reason != "0": raise ValueError("valid sample must have success reason")
            line = op + " " + integer(step.get("source"), 1, 65535) + " " + status + " " + reason
        elif op == "sensor_reset":
            fields |= {"source"}
            line = op + " " + integer(step.get("source"), 1, 65535)
        else:
            raise ValueError("unknown scenario operation")
        if set(step) - fields:
            raise ValueError("unknown scenario step fields")
        lines.append(line)
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init", help="create a declarative watchface project")
    init.add_argument("directory", type=Path)
    init.add_argument("--id", required=True)
    init.add_argument("--publisher", required=True)
    build = commands.add_parser("build", help="compile and validate before replacing output")
    build.add_argument("project", type=Path)
    build.add_argument("output", type=Path)
    build.add_argument("--validator", type=Path, default=Path(__file__).with_name(
        "ignite-check.exe" if os.name == "nt" else "ignite-check"))
    check = commands.add_parser("check", help="validate an existing package and print JSON report")
    check.add_argument("package", type=Path)
    check.add_argument("--validator", type=Path, default=Path(__file__).with_name(
        "ignite-check.exe" if os.name == "nt" else "ignite-check"))
    run = commands.add_parser("run", help="simulate Lua frames and save a PPM image")
    run.add_argument("package", type=Path)
    run.add_argument("--frames", type=int, default=120)
    run.add_argument("--press-every", type=int, default=24)
    run.add_argument("--image", type=Path, required=True)
    run.add_argument("--scenario", type=Path, help="replay bounded JSON steps through Core")
    run.add_argument("--storage", type=Path, help="retain simulator private state between runs")
    run.add_argument("--factory", type=Path, help="factory packages for recovery scenarios")
    run.add_argument("--runner", type=Path, default=Path(__file__).with_name(
        "ignite-run.exe" if os.name == "nt" else "ignite-run"))
    args = parser.parse_args(argv)
    try:
        if args.command == "init":
            identifier(args.id)
            identifier(args.publisher)
            project = {
                "id": args.id, "name": "My watchface", "publisher": args.publisher,
                "version": 1,
                "components": [{"id": "main", "type": "watchface",
                    "budget": {"internal": 24576, "external": 0, "instructions": 4096},
                    "capabilities": 1, "permissions": ["ui.render", "time.read"], "ui": [
                    {"kind": "rect", "w": 280, "h": 240, "color": "theme.background"},
                    {"kind": "text", "x": 140, "y": 120, "center": True,
                     "text": "${time.read}", "color": "theme.text"},
                ]}],
            }
            # Refuse to overwrite user files, including nonempty existing directories.
            args.directory.mkdir(parents=True, exist_ok=False)
            (args.directory / "project.json").write_text(
                json.dumps(project, ensure_ascii=False, indent=4) + "\n", encoding="utf-8"
            )
            print(args.directory / "project.json")
            return 0
        if args.command == "run":
            if not 1 <= args.frames <= 3600 or not 0 <= args.press_every <= 3600:
                raise ValueError("frames must be 1..3600; press interval 0..3600")
            if args.image.resolve() == args.package.resolve():
                raise ValueError("image must not overwrite package")
            if (args.storage or args.factory) and not args.scenario:
                raise ValueError("--storage and --factory require --scenario")
            script = scenario_script(args.scenario) if args.scenario else None
            if args.scenario and args.image.resolve() == args.scenario.resolve():
                raise ValueError("image must not overwrite scenario")
            temporary_root = Path(__file__).resolve().parents[1] / "temp"
            temporary_root.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="ignite-scenario-", dir=temporary_root) as directory:
                command = [str(args.runner.resolve()), str(args.package.resolve()),
                    str(args.frames), str(args.press_every), str(args.image.resolve())]
                if script is not None:
                    storage = args.storage.resolve() if args.storage else Path(directory) / "store"
                    factory = args.factory.resolve() if args.factory else Path(directory) / "factory"
                    command += ["--scenario", str(storage), str(factory)]
                environment = os.environ.copy()
                environment.update(TMPDIR=directory, TEMP=directory, TMP=directory)
                result = subprocess.run(command, input=script, env=environment,
                    capture_output=True, text=True, encoding="utf-8")
            if result.returncode:
                raise ValueError(result.stderr.strip() or "simulation failed")
            print(result.stdout, end="")
            return 0
        if args.command == "check":
            report = validate(args.package, args.validator.resolve())
        else:
            project = json.loads(args.project.read_text(encoding="utf-8"))
            data = compile_project(project, args.project.resolve().parent)
            if args.output.resolve() == args.project.resolve():
                raise ValueError("output must not overwrite the source project")
            # Validation must succeed before the old output is touched.
            with tempfile.TemporaryDirectory() as directory:
                candidate = Path(directory) / "candidate.ignit"
                candidate.write_bytes(data)
                report = validate(candidate, args.validator.resolve())
            publish_file(args.output, data)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, OverflowError, struct.error) as error:
        print(f"Ignite SDK: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    # Reports and diagnostics have the same UTF-8 contract as the native validator.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    sys.exit(main())
