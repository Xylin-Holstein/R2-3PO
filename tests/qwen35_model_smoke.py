#!/usr/bin/env python3
"""Contract checks for the current unified Gemma conversation/vision integration."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


core = (ROOT / "r2.c").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

require(
    re.search(r'^static char MODEL\[256\] = "gemma3:4b";$', core, re.MULTILINE) is not None,
    "the C conversation core defaults to Gemma 3 4B",
)
require(
    'const char *requested = getenv("R2_MODEL");' in core
    and "configure_conversation_model()" in core
    and 'json_object_new_string(MODEL)' in core
    and 'http://127.0.0.1:11434/api/chat' in core,
    "R2's C kernel sends its configured model name to the local Ollama chat API",
)
require(
    '#define R2_VISION_DEFAULT_MODEL "gemma3:4b"' in visual
    and 'getenv("R2_VISION_MODEL")' in visual,
    "visual perception uses Gemma by default and accepts the launcher-provided model",
)
require(
    'R2_MODEL="${R2_MODEL:-gemma3:4b}"' in launcher
    and 'R2_VISION_MODEL="$R2_MODEL"' in launcher
    and 'R2_MODEL="$R2_MODEL"' in launcher
    and 'R2_VISION_MODEL="$R2_VISION_MODEL"' in launcher,
    "the launcher passes one selected model identity to both conversation and vision",
)
require(
    'R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"' in launcher
    and 'sudo -u r2 env R2_MODEL="$R2_MODEL" R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL"' in launcher
    and "preflight_ollama_model.py" in launcher
    and 'systemctl show r2-ollama.service -p User' in launcher,
    "preflight checks that the selected model is installed before launching R2 as Linux user r2",
)
require(
    '"Reality.h"' in core and '"Reward.h"' in core and '"R2Reply.h"' in core
    and '"R2Sounds.h"' in core
    and "append_runtime_context(&memory_context)" in core
    and "r2_reality_autonomous_feed_if_needed()" in core,
    "the model is wired to live Reality/learning context, reply processing, sounds, and the needs worker",
)
require(
    "static int split_tool_fields" in core
    and 'extract_marker(reply, "[REALITY_ADD]"' in core
    and 'extract_marker(reply, "[FRIDGE_EAT]"' in core
    and 'extract_marker(reply, "[MONEY_DEPOSIT]"' in core
    and "strip_action_markers" in core,
    "model tool markers invoke C-backed world operations and are removed from user-facing replies",
)
require(
    "legacy_copy_table" in reality
    and "r2_reality_item_memory" in reality
    and "r2_food_experiences" in reality
    and "r2_food_preferences" in reality
    and "r2_money_account" in reality,
    "the connected Reality engine preserves legacy inventory, food history/preferences, and money",
)
print("Unified Gemma conversation/vision contract checks passed.")
