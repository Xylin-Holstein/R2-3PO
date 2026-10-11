#!/usr/bin/env python3
"""Safety/coverage contract for the non-destructive hardware preflight."""
from pathlib import Path

script = Path("tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
required = (
    'set -uo pipefail',
    'R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"',
    'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"',
    'http://127.0.0.1:11434/api/tags',
    'gemma3:4b',
    'PRAGMA integrity_check',
    '?mode=ro',
    'r2_memory.db', 'r2_reality.db', 'r2_fridge.db',
    'r2_rewards.db', 'r2_addictions.db',
    'pactl', '/dev/video*', 'mGBA Qt emulator',
    'stop it before installation or database preflight',
    'sudo -n -u r2 test -r "$R2_FX_DIR"',
)
missing = [item for item in required if item not in script]
if missing:
    raise SystemExit(f"preflight contract missing expected checks: {missing}")
for forbidden in ('chmod 777', 'apt-get install', 'apt-get update',
                  'systemctl enable', 'systemctl start', 'systemctl stop',
                  'make clean all', 'rm -f'):
    if forbidden in script:
        raise SystemExit(f"preflight must be read-only; found forbidden operation: {forbidden}")
if 'R2_OLLAMA_URL:-http://127.0.0.1:11434/api/tags' not in script:
    raise SystemExit("Ollama endpoint must remain locally configurable")
print("R2 hardware preflight safety/coverage smoke test passed")
