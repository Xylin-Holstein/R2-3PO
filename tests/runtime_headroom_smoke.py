#!/usr/bin/env python3
"""Guard foreground scheduling, RAM-aware context, and additive memory migration."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
diary = (ROOT / "r2_diary.c").read_text(encoding="utf-8")
life_log = (ROOT / "Log.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
alternate_self = (ROOT / "AlternateSelf.c").read_text(encoding="utf-8")

required = {
    "RAM-aware context selection": "static int ollama_context_tokens(void)",
    "Ollama receives explicit context length": '"num_ctx", json_object_new_int(num_ctx)',
    "Output budget is capped to context headroom": "if (num_predict > max_predict) num_predict = max_predict;",
    "Legacy memories are migrated additively": "static int migrate_memory_schema(void)",
    "Legacy content aliases are preserved": '"content", "memory_text", "text", "description", "value"',
    "Current-turn reserve prevents old context crowding it out": "size_t current_user_reserve = 1500;",
    "Live history has a byte ceiling": "#define MAX_LIVE_MESSAGE_BYTES (32U * 1024U * 1024U)",
    "Autonomous reflection uses the background scheduler": "previous_background_mode = ollama_background_mode",
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
if not re.search(
    r"static void \*autonomous_thinking\([\s\S]{0,350}ollama_background_mode\s*=\s*1;",
    core,
):
    raise SystemExit(
        "Autonomous post-reflection extraction is not kept in background mode; "
        "it could block a live conversation after the reflection itself yields."
    )
if "int previous_background_mode = ollama_background_mode;" not in reflection:
    raise SystemExit("Autonomous reflection must restore the thread's previous mode")

for label, source, needle in (
    ("fridge schema migration", reality, "static int fridge_migrate_schema(void)"),
    ("Reality need-state migration", reality, "static int migrate_reality_self_schema(void)"),
    ("visual experience schema migration", visual, "static int visual_migrate_schema(void)"),
    ("diary link schema migration", diary, "diary_link_ensure_column(\"memory_indexed\""),
    ("Life Log schema migration", life_log, "static int migrate_legacy_log_schema(void)"),
    ("repeated-interest schema migration", addiction, "static int migrate_addiction_schema(void)"),
    ("Alternate-Self schema migration", alternate_self, "static int migrate_altself_schema(void)"),
):
    if needle not in source:
        raise SystemExit(f"Missing backwards-compatibility safeguard: {label}")

if any("DROP TABLE" in source.upper() for source in (core, reality, visual, diary, life_log, addiction, alternate_self)):
    raise SystemExit("R2 schema migrations must never drop persistent tables")
if "DELETE FROM r2_alternate_self_branches" in alternate_self:
    raise SystemExit("Alternate-Self migration must preserve existing branches")
print("Runtime headroom and legacy memory safeguards passed.")
