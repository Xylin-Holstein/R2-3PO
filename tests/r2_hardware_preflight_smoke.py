#!/usr/bin/env python3
"""Safety/coverage contract for the read-only hardware preflight and private model service."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
app_service = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
model_service = (ROOT / "deploy/r2-ollama.service").read_text(encoding="utf-8")
installer = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")

required = (
    'set -uo pipefail',
    'R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"',
    'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"',
    'http://127.0.0.1:11434/api/tags',
    'gemma3:4b',
    'PRAGMA integrity_check', '?mode=ro',
    'r2_memory.db', 'r2_reality.db', 'r2_fridge.db', 'r2_rewards.db', 'r2_addictions.db',
    'pactl', '/dev/video*', '/dev/snd/*',
    'r2_diary.c r2_diary.h', 'Reality.c Reality.h', 'Addiction.c Addiction.h',
    'Reward.c Reward.h', 'AlternateSelf.c AlternateSelf.h', 'R2Sounds.c R2Sounds.h',
    'R2Reply.c R2Reply.h', 'shell.c shell.h', 'Observer.py',
    'tools/audit_r2_sound_assets.py', 'tools/preflight_r2_hardware.sh',
    'libfaketime.so.1 is missing', 'overnight service installation cannot proceed',
)
missing = [item for item in required if item not in script]
if missing:
    raise SystemExit(f"preflight contract missing expected checks: {missing}")
if "SupplementaryGroups=audio" not in app_service:
    raise SystemExit("R2 service must receive service-scoped audio access")
if "User=r2" not in model_service or "OLLAMA_HOST=127.0.0.1:11435" not in model_service:
    raise SystemExit("dedicated Ollama inference must run as r2 on localhost port 11435")
if "--uid-owner r2" not in model_service or "--dport 11435" not in model_service or "ip6tables" not in model_service:
    raise SystemExit("model service must enforce IPv4 and IPv6 egress isolation")
if "copy_ollama_model.py" not in installer or "r2-ollama.service" not in installer:
    raise SystemExit("installer must copy the local model and install the private model service")
for forbidden in ("chmod 777", "systemctl enable", "systemctl start", "systemctl stop", "make clean all", "rm -f"):
    if forbidden in script:
        raise SystemExit(f"preflight must be read-only; found forbidden operation: {forbidden}")
for line in script.splitlines():
    if line.lstrip().startswith(("apt-get update", "apt-get install", "sudo apt-get update", "sudo apt-get install")):
        raise SystemExit("preflight must not execute package-manager commands")
if "ensure_dir_tree(" not in reality or "R2_ROOT" not in reality:
    raise SystemExit("Reality must create runtime directories under the configured R2 root")
launch_required = (
    'R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"',
    'bash "$R2_PREFLIGHT"',
    'R2 launch cancelled: preflight reported one or more failures.',
    'Press Enter to continue launching R2, or Ctrl+C to cancel...',
)
missing_launch = [item for item in launch_required if item not in launcher]
if missing_launch:
    raise SystemExit(f"launcher does not run/guard startup preflight: {missing_launch}")
if 'R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"' not in launcher:
    raise SystemExit("interactive launch must use the dedicated private Ollama endpoint")
if "systemctl show r2-ollama.service -p User" not in launcher or 'sudo -u r2 \\' not in launcher:
    raise SystemExit("launcher must verify the inference service user and run R2 as Linux user r2")
if "R2_OLLAMA_BASE_URL" not in core or "gemma3:4b" not in core:
    raise SystemExit("C core must connect to the configured local Gemma endpoint")
print("R2 hardware preflight and private model-service smoke test passed")
