#!/usr/bin/env python3
"""Safety/coverage contract for the non-destructive hardware preflight."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
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
    'pactl', 'sudo -n -u r2 pactl info',
    '/dev/video*', 'R2 runtime account can read/write',
    '/dev/snd/*', 'mGBA Qt emulator',
    'stop it before installation or database preflight',
    'sudo -n -u r2 test -r "$R2_FX_DIR"',
    'r2_diary.c r2_diary.h',
    'Reality.c Reality.h',
    'Addiction.c Addiction.h',
    'Reward.c Reward.h',
    'AlternateSelf.c AlternateSelf.h',
    'R2Sounds.c R2Sounds.h',
    'R2Reply.c R2Reply.h',
    'shell.c shell.h',
    'Observer.py',
    'tools/audit_r2_sound_assets.py',
    'tools/preflight_r2_hardware.sh',
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

launch_required = (
    'R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"',
    'bash "$R2_PREFLIGHT"',
    'R2 launch cancelled: preflight reported one or more failures.',
    'Press Enter to continue launching R2, or Ctrl+C to cancel...',
)
missing_launch = [item for item in launch_required if item not in launcher]
if missing_launch:
    raise SystemExit(f"launcher does not run/guard the startup preflight: {missing_launch}")
# The preflight must run before duplicate-process checks, directory creation,
# model checks, package installation, compilation, and observer startup.
preflight_at = launcher.index('bash "$R2_PREFLIGHT"')
for marker in (
    'systemctl is-active --quiet r2-3po.service',
    'mkdir -p "$R2_ROOT"',
    'Checking the local Ollama API and required model',
    'sudo apt-get install -y libfaketime',
    'Recompiling R2',
    'Launching the read-only observer window',
):
    if launcher.index(marker) < preflight_at:
        raise SystemExit(f"startup preflight must precede {marker!r}")
print("R2 hardware preflight safety, coverage, and launcher-order smoke test passed")
