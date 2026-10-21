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
    "RAM-aware context selection": "int r2_ollama_context_tokens(void)",
    "Ollama receives explicit context length": '"num_ctx", json_object_new_int(num_ctx)',
    "Output budget is capped to context headroom": "if (num_predict > max_predict) num_predict = max_predict;",
    "Legacy memories are migrated additively": "static int migrate_memory_schema(void)",
    "Legacy content aliases are preserved": '"content", "memory_text", "text", "description", "value"',
    "Current-turn reserve follows the actual serialized message": "current_user_reserve = current_length;",
    "Current-turn reserve does not waste a fixed 1500-character block": "size_t current_user_reserve = 0;",
    "Live history has a byte ceiling": "#define MAX_LIVE_MESSAGE_BYTES (32U * 1024U * 1024U)",
    "Autonomous reflection uses the background scheduler": "previous_background_mode = ollama_background_mode",
    "cgroup RAM reads distinguish zero from unreadable": "static unsigned long long read_uint_file(const char *path, int *valid)",
    "cgroup RAM exhaustion forces minimal headroom": "(!cgroup_used_valid || cgroup_used >= cgroup_max)",
    "unlimited cgroup v1 sentinel is not treated as real headroom": "v1_max < (1ULL << 60)",
    "foreground timeout is configurable for long local inference": "R2_OLLAMA_TIMEOUT_SECONDS",
    "foreground timeout has a six-hour safety ceiling": "requested <= 21600L",
    "foreground timeout defaults to six hours for slow local inference": "const long default_timeout = 21600L;",
}
for label, needle in required.items():
    if needle not in core:
        raise SystemExit(f"Missing runtime headroom safeguard: {label}")

if "int r2_ollama_context_tokens(void);" not in (ROOT / "r2.h").read_text(encoding="utf-8"):
    raise SystemExit("Shared RAM-aware context sizing must be part of the core interface.")
if "int num_ctx = r2_ollama_context_tokens();" not in visual:
    raise SystemExit("Vision must use the shared RAM-aware context selector.")

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

talk_start = core.find("static char *r2_talk_impl(const char *message)")
talk_end = core.find("\nchar *r2_talk(const char *message)", talk_start)
if talk_start < 0 or talk_end < 0:
    raise SystemExit("Could not locate the serialized conversation-turn implementation.")
talk = core[talk_start:talk_end]
retry_marker = talk.find('"conversation_tool_followup_retry"')
if retry_marker < 0:
    raise SystemExit("A failed post-action reply must trigger one logged recovery attempt.")
retry_refresh = talk.find("(void)refresh_reality_context_message();", retry_marker)
retry_generation = talk.find("reply = chat_with_relevant_memories(message);", retry_refresh)
final_failure = talk.find("Follow-up generation failed after one refreshed retry", retry_generation)
if min(retry_refresh, retry_generation, final_failure) < 0 or not (retry_marker < retry_refresh < retry_generation < final_failure):
    raise SystemExit("Post-action recovery must refresh context, retry generation, then roll back only if retry fails.")
if talk.count("process_tools(") != 1:
    raise SystemExit("A failed post-action reply retry must not execute already-completed tools a second time.")

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
print("Runtime headroom and legacy memory compatibility checks passed.")
