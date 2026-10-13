#!/usr/bin/env python3
"""Safety/coverage contract for the non-destructive hardware preflight."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
app_service = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
model_service = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8")
installer = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")

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
    'pactl', 'R2 runtime account can connect to its default PulseAudio/PipeWire server',
    '/dev/video*', 'R2 runtime account can read/write',
    '/dev/snd/*',
    "R2 service's configured audio supplementary group can access", 'mGBA Qt emulator',
    'stop it before installation or database preflight',
    'run_as_r2 test -r "$R2_FX_DIR"',
    'r2 must have read/write/traverse access to $path for runtime directories, SQLite, and persistent memory',
    'existing runtime directory is not read/write/traverse accessible to r2',
    'existing database must be a readable/writable regular file for r2',
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
    'for tool in bash make gcc curl python3 sudo setpriv pgrep date find grep cmp iptables ip6tables',
    'run_as_r2_with_audio_group()',
    'Hardware preflight identity: %s (uid=%s, groups=%s)',
    'DROP=(setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" --)',
    'sudo -n "${DROP[@]}" env HOME="$R2_ROOT" USER=r2 LOGNAME=r2',
    'sudo -n setpriv --reuid="$uid" --regid="$gid" --groups="$groups" -- "$@"',
    'libfaketime.so.1 is missing',
    'sudo apt-get update && sudo apt-get install -y ffmpeg',
    'sudo apt-get update && sudo apt-get install -y pulseaudio-utils',
    'physical-device/configuration issue, not a missing package',
    'overnight service installation cannot proceed',
)
missing = [item for item in required if item not in script]
if missing:
    raise SystemExit(f"preflight contract missing expected checks: {missing}")

if "SupplementaryGroups=audio" not in app_service:
    raise SystemExit("R2 service must receive service-scoped ALSA access through the audio group")
if "User=r2" not in model_service or "OLLAMA_HOST=127.0.0.1:11435" not in model_service:
    raise SystemExit("the dedicated Ollama inference process must run as r2 on its private localhost endpoint")
if "--uid-owner r2" not in model_service or "--dport 11435" not in model_service or "ip6tables" not in model_service:
    raise SystemExit("the dedicated model service must enforce IPv4 and IPv6 egress isolation for UID r2")
if "copy_ollama_model.py" not in installer or "r2-ollama.service" not in installer:
    raise SystemExit("installer must copy the local model into R2's private store and install the dedicated service")

for forbidden in ("chmod 777", "systemctl enable", "systemctl start", "systemctl stop", "make clean all", "rm -f"):
    if forbidden in script:
        raise SystemExit(f"preflight must be read-only; found forbidden operation: {forbidden}")
for line in script.splitlines():
    if line.lstrip().startswith(("apt-get update", "apt-get install", "sudo apt-get update", "sudo apt-get install")):
        raise SystemExit("preflight must not execute package-manager commands")
if 'sudo -n -u r2 -g r2 -G audio' in script:
    raise SystemExit("preflight must not use unsupported sudo-rs -G syntax")
if 'sudo -n -u r2 test -r' in script or 'sudo -n -u r2 pactl info' in script:
    raise SystemExit("runtime checks must execute directly as r2 after the preflight privilege drop")
if 'sudo -n setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" --' not in launcher:
    raise SystemExit("launcher must use setpriv for supplementary groups under sudo-rs")
if 'sudo -u r2 -g r2 -G "$R2_RUNTIME_GROUPS"' in launcher:
    raise SystemExit("launcher must not use unsupported sudo-rs -G syntax")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
if "ensure_dir_tree(" not in core or "R2_ROOT" not in core:
    raise SystemExit("runtime directories must be created by the core process running as r2")
if 'R2_OLLAMA_URL:-http://127.0.0.1:11434/api/tags' not in script:
    raise SystemExit("hardware preflight must inspect the existing local model store/API before the model is copied")

launch_required = (
    'R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"',
    'bash "$R2_PREFLIGHT"',
    'R2 launch cancelled: preflight reported one or more failures.',
    'Press Enter to continue launching R2, or Ctrl+C to cancel...',
)
missing_launch = [item for item in launch_required if item not in launcher]
if missing_launch:
    raise SystemExit(f"launcher does not run/guard the startup preflight: {missing_launch}")
if "R2_OLLAMA_BASE_URL=\"${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}\"" not in launcher:
    raise SystemExit("interactive launch must use the dedicated r2-owned Ollama endpoint")
if "systemctl show r2-ollama.service -p User" not in launcher:
    raise SystemExit("interactive launch must verify that the model service runs as r2")
print("R2 hardware preflight safety, coverage, and dedicated model-service smoke test passed")
