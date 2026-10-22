#!/usr/bin/env python3
"""Guard against dropping the active user turn during context-based Ollama history selection."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *ollama_chat_with_limit(")
end = core.find("\nstatic char *ollama_chat(", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the Ollama request builder.")
builder = core[start:end]

required = (
    "size_t current_user_index = SIZE_MAX;",
    'if (!msgs[index].role || strcmp(msgs[index].role, "user") != 0)',
    '"----- BEGIN CURRENT USER MESSAGE -----\\n"',
    '"----- END CURRENT USER MESSAGE -----"',
    "include[current_user_index] = 1;",
    "limits[current_user_index] = length;",
    "size_t remaining = input_budget - total_chars;",
    "const char *authoritative_query = ollama_authoritative_user_query;",
    "strcmp(content, authoritative_query) == 0",
    "memcmp(begin, authoritative_query, wrapped_length) == 0",
    "if (wrapped_length && begin[wrapped_length - 1] == '\\n')",
    "if (current_user_index == SIZE_MAX && authoritative_query && *authoritative_query)",
    '"ollama_current_user_turn_missing"',
    "active user turn was absent from the serialized history",
    "current_user_index = fallback_user_index;",
)
for needle in required:
    if needle not in builder:
        raise SystemExit(f"Current-turn retention behavior missing: {needle}")

explicit_turn = builder.find("include[current_user_index] = 1;")
history_loop = builder.find("for (size_t i = count; i > 0; --i)")
if explicit_turn < 0 or history_loop < 0 or explicit_turn > history_loop:
    raise SystemExit("The current user turn must be budgeted before older history.")

if "OLLAMA_MAX_MESSAGE_CHARS" in builder or "OLLAMA_MAX_RECENT_MESSAGES" in builder:
    raise SystemExit("Inference payload still has an arbitrary per-message or recent-turn cap.")
if "size_t input_budget = (size_t)(num_ctx - num_predict - 512) * 32U / 10U;" not in builder:
    raise SystemExit("Inference payload must allocate from the actual Ollama context window.")
if "if (length > remaining) length = r2_utf8_safe_prefix_length(content, remaining);" not in builder:
    raise SystemExit("History allocation must use the real remaining context budget.")

if "static _Thread_local const char *ollama_authoritative_user_query = NULL;" not in core:
    raise SystemExit("The active user query must be tracked per thread, not in shared global state.")
if "ollama_authoritative_user_query = query;" not in core:
    raise SystemExit("Conversation inference must pass the authoritative user query into payload selection.")
if "ollama_authoritative_user_query = previous_authoritative_query;" not in core:
    raise SystemExit("The authoritative query binding must be restored after inference.")

print("Ollama current-turn retention smoke passed.")
