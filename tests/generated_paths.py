"""Locations of packages generated for the active CMake test build."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FACTORY_DIR = Path(os.environ.get("FLINT_FACTORY_DIR", ROOT / "build/generated/factory"))
EXAMPLES_DIR = Path(os.environ.get("FLINT_EXAMPLES_DIR", ROOT / "build/generated/examples"))
