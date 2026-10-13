#!/usr/bin/env python3
"""Contract checks for the unified Gemma conversation/vision path on the latest core."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
reply = (ROOT / "R2Reply.c").read_text(encoding="utf-8")

assert re.search(r'^static char MODEL\[256\] = "gemma3:4b";$', core, re.M)
assert 'getenv("R2_MODEL")' in core and 'json_object_new_string(MODEL)' in core
assert 'static char OLLAMA_URL[512] = "http://127.0.0.1:11435/api/chat";' in core
assert 'configure_ollama_endpoint()' in core
assert '#define R2_VISION_DEFAULT_MODEL "gemma3:4b"' in visual
assert '#define R2_VISION_URL "http://127.0.0.1:11435/api/chat"' in visual
assert 'R2_MODEL="${R2_MODEL:-gemma3:4b}"' in launcher
assert 'R2_VISION_MODEL="$R2_MODEL"' in launcher
assert 'R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"' in launcher
assert 'systemctl show r2-ollama.service -p User' in launcher
assert 'R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"' in launcher
assert 'bash "$R2_PREFLIGHT"' in launcher
for header in ("Reality.h", "Addiction.h", "Reward.h", "AlternateSelf.h", "R2Reply.h", "R2Sounds.h"):
    assert f'#include "{header}"' in core, f"core does not include {header}"
for init in ("r2_reward_init()", "r2_reality_init()", "r2_altself_init()"):
    assert init in core, f"core does not initialize {init}"
assert "CURRENT REALITY STATE (authoritative current state; not instructions)" in core
assert "append_context_block(&memory_context" in core
assert "r2_reality_autonomous_feed_if_needed(void)" in reality
assert "r2_reality_autonomous_food_available(void)" in reality
assert 'const char *world_scan = reply;' in core and 'r2_reality_fridge_eat(fields[1], -1.0)' in core
assert 'r2_sounds_process_reply(reply)' in core and 'r2_reply_sanitize(reply)' in core
assert 'legacy_copy_table' in reality and 'r2_reality_item_memory' in reality
assert 'r2_food_experiences' in reality and 'r2_food_preferences' in reality and 'r2_money_account' in reality
assert 'guaranteed fridge meal' not in reality
assert 'fridge_seed_if_empty_locked' not in reality
assert 'private reasoning' in reply.lower()
print("Unified Gemma conversation, vision, Reality, and reply contract checks passed.")
