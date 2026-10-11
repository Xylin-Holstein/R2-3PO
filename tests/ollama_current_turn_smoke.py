#!/usr/bin/env python3
"""Guard against dropping the active user turn during bounded Ollama history selection."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *ollama_chat_with_limit(")
end = core.find("\nstatic char *ollama_chat(", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the bounded Ollama request builder.")
builder = core[start:end]

required = (
    "size_t current_user_index = SIZE_MAX;",
    'if (msgs[index].role && !strcmp(msgs[index].role, "user"))',
    '"----- BEGIN CURRENT USER MESSAGE -----\\n"',
    '"----- END CURRENT USER MESSAGE -----"',
    "include[current_user_index] = 1;",
    "limits[current_user_index] = length;",
    "size_t remaining = input_budget - total_chars;",
)
for needle in required:
    if needle not in builder:
        raise SystemExit(f"Current-turn retention safeguard missing: {needle}")

explicit_turn = builder.find("include[current_user_index] = 1;")
history_loop = builder.find("for (size_t i = count; i > 0 && recent_count < OLLAMA_MAX_RECENT_MESSAGES; --i)")
if explicit_turn < 0 or history_loop < 0 or explicit_turn > history_loop:
    raise SystemExit("The current user turn must be budgeted before recent-history selection.")

if "recent_count = 1;" not in builder:
    raise SystemExit("The explicitly included user turn must count against the recent-message limit.")
if "if (include[index]) continue;" not in builder:
    raise SystemExit("Recent-history selection must not duplicate the explicitly included user turn.")
if "if (length > OLLAMA_MAX_MESSAGE_CHARS)" not in builder:
    raise SystemExit("The explicit current turn must retain the per-message safety cap.")

print("Ollama current-turn retention smoke passed.")
