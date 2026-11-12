#!/usr/bin/env python3
"""Compile factory and example packages into output directories, never source fixtures."""

import argparse
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from compile_ignite import compile_project
from generation import write_if_changed

ROOT = Path(__file__).resolve().parents[1]


def build_packages(output: Path, examples_output: Path | None = None) -> None:
    # Compile every input before publishing or pruning any prior successful output.
    packages = {}
    for source in sorted((ROOT / "system").glob("*.json")):
        packages[source.stem + ".ignit"] = compile_project(
            json.loads(source.read_text(encoding="utf-8")), source.parent)
    examples = {}
    if examples_output is not None:
        for source in sorted((ROOT / "examples").glob("*/project.json")):
            examples[source.parent.name] = compile_project(
                json.loads(source.read_text(encoding="utf-8")), source.parent)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in packages.items():
        write_if_changed(output / name, data)
    for name, data in examples.items():
        write_if_changed(examples_output / name / "sample.ignit", data)
    if examples_output is not None:
        for path in examples_output.glob("*/sample.ignit"):
            if path.parent.name not in examples:
                path.unlink()
                if not any(path.parent.iterdir()):
                    path.parent.rmdir()
    # This directory is generated output; preserve unknown/non-package files.
    for path in output.glob("*.ignit"):
        if path.name not in packages:
            path.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated/factory")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--examples-output", type=Path,
                       default=ROOT / "build/generated/examples")
    group.add_argument("--skip-examples", action="store_true",
                       help="only compile factory packages (firmware integration)")
    args = parser.parse_args(argv)
    build_packages(args.output, None if args.skip_examples else args.examples_output)


if __name__ == "__main__":
    main()
