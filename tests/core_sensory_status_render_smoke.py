#!/usr/bin/env python3
"""Prevent core status from reporting unavailable sensory devices as open."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")
start = source.index("void r2_status(void)")
end = source.index("void r2_diagnostics(void)", start)
status = source[start:end]

assert 'r2_eyes_status() > 0 ? "OPEN"' in status
assert 'r2_eyes_status() == 0 ? "CLOSED" : "UNAVAILABLE"' in status
assert 'r2_ears_status() > 0 ? "OPEN"' in status
assert 'r2_ears_status() == 0 ? "CLOSED" : "UNAVAILABLE"' in status
print("Core status distinguishes open, closed, and unavailable sensory devices.")
