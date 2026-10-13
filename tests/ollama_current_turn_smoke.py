#!/usr/bin/env python3
"""Guard the actual Ollama request builder against dropping the active user turn."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *ollama_chat(")
end = core.find("\n\n/* ============================================================\n   NORMAL CONVERSATION", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the active Ollama request builder.")
builder = core[start:end]

required = (
    "size_t current_user_index = SIZE_MAX;",
    "size_t current_user_length = 0;",
    'strcmp(msgs[index].role, "user") == 0',
    "current_user_index = index;",
    "size_t current_user_reserve = current_user_length;",
    "if (current_user_reserve < 1500) current_user_reserve = 1500;",
    "if (current_user_reserve > OLLAMA_MAX_MESSAGE_CHARS)",
    "size_t max_user_reserve = input_budget > 1500",
    "if (current_user_reserve > max_user_reserve)",
    "size_t remaining = input_budget > total_chars",
    "include[current_user_index] = 1;",
    "limits[current_user_index] = length;",
    "++recent_count;",
    "if (include[index] || !msgs[index].role",
    "if (length > OLLAMA_MAX_MESSAGE_CHARS)",
    "for (size_t i = count; i > 0 && recent_count < OLLAMA_MAX_RECENT_MESSAGES; --i)",
)
for needle in required:
    if needle not in builder:
        raise SystemExit(f"Current-turn retention safeguard missing: {needle}")

explicit_turn = builder.find("include[current_user_index] = 1;")
history_loop = builder.find(
    "for (size_t i = count; i > 0 && recent_count < OLLAMA_MAX_RECENT_MESSAGES; --i)"
)
if explicit_turn < 0 or history_loop < 0 or explicit_turn > history_loop:
    raise SystemExit("The current user turn must be budgeted before recent-history selection.")

# The last user-role message is identified by reverse scan, then reserved before
# older turns. It is counted against the history cap and excluded from the
# second pass, preventing either crowd-out or duplicate serialization.
if builder.find("current_user_index = index;") > explicit_turn:
    raise SystemExit("The current user turn must be located before allocation.")
if builder.find("++recent_count;", explicit_turn) < 0:
    raise SystemExit("The explicitly included current turn must count toward the message limit.")
if "OLLAMA_MAX_TOTAL_CHARS 47000" not in core:
    raise SystemExit("The serialized request must retain its total character ceiling.")
if "OLLAMA_MAX_SYSTEM_TOTAL_CHARS 41000" not in core:
    raise SystemExit("System context must retain its separate aggregate ceiling.")
if "OLLAMA_MAX_SYSTEM_MESSAGE_CHARS 24000" not in core:
    raise SystemExit("Each system message must retain its per-message ceiling.")

print("Ollama current-turn retention smoke passed.")
