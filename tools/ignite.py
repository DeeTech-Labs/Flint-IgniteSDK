#!/usr/bin/env python3
"""Independent Ignite SDK CLI. Uses the shared native validator, never a second parser."""

import argparse
import json
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
                "budget": {"internal": 24576, "external": 0, "instructions": 4096},
                "permissions": ["ui.render", "time.read"],
                "components": [{"id": "main", "type": "watchface", "ui": [
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
            result = subprocess.run([str(args.runner.resolve()), str(args.package.resolve()),
                str(args.frames), str(args.press_every), str(args.image.resolve())],
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
