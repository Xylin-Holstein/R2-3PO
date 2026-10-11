#!/usr/bin/env python3
"""Guard foreground scheduling, RAM-aware context, and additive memory migration."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

required = {
    "RAM-aware context selection": "static int ollama_context_tokens(void)",
    "Ollama receives explicit context length": '"num_ctx", json_object_new_int(num_ctx)',
    "Output budget is capped to context headroom": "if (num_predict > max_predict) num_predict = max_predict;",
    "Legacy memories are migrated additively": "static int migrate_memory_schema(void)",
    "Legacy content aliases are preserved": '"content", "memory_text", "text", "description", "value"',
    "Current-turn reserve prevents old context crowding it out": "size_t current_user_reserve = 3000;",
}
for label, needle in required.items():
    if needle not in core:
        raise SystemExit(f"Missing runtime headroom safeguard: {label}")

reflection_start = core.find("static void *autonomous_thinking(")
reflection_end = core.find("/* ============================================================", reflection_start + 40)
if reflection_start < 0:
    raise SystemExit("Could not find autonomous reflection worker")
reflection = core[reflection_start:reflection_end if reflection_end > reflection_start else reflection_start + 12000]
if not re.search(
    r"ollama_background_mode\s*=\s*1;[\s\S]{0,500}ollama_chat_with_limit\(",
    reflection,
):
    raise SystemExit(
        "Autonomous reflection does not mark its model call as background work; "
        "it could block a live conversation."
    )
if "int previous_background_mode = ollama_background_mode;" not in reflection:
    raise SystemExit("Autonomous reflection must restore the thread's previous mode")

if "DROP TABLE" in core.upper():
    raise SystemExit("Core memory migration must never drop a table")
print("Runtime headroom and legacy memory safeguards passed.")
