#!/usr/bin/env python3
"""Installed entry point for the hardware matrix collector."""
from pathlib import Path
import runpy
import sys

directory = Path(__file__).resolve().parents[1] / "libexec/ciderdarling"
sys.path.insert(0, str(directory))
runpy.run_path(str(directory / "hardware-matrix.py"), run_name="__main__")
