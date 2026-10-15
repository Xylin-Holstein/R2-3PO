#!/usr/bin/env python3
"""Ensure the launcher validates every newly required conversation input header."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
launcher = (root / "R2_Launch_Code.sh").read_text(encoding="utf-8")
required_start = launcher.find("REQUIRED_FILES=(")
required_end = launcher.find("\n)", required_start)
if required_start < 0 or required_end < 0:
    raise SystemExit("FAIL: launcher REQUIRED_FILES array could not be isolated")
required = launcher[required_start:required_end]
for source in ("r2.c", "shell.c", "r2_input.h"):
    if f'"$R2_SOURCE/{source}"' not in required:
        raise SystemExit(f"FAIL: launcher does not preflight {source}")
if '"$R2_SOURCE/r2.c"' not in launcher or '"$R2_SOURCE/shell.c"' not in launcher:
    raise SystemExit("FAIL: launcher compile command does not include core and shell")
print("Launcher input-header preflight smoke passed.")
