#!/usr/bin/env python3
"""Prevent inference when adaptive payload limits would truncate the active turn."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "r2.c").read_text(encoding="utf-8")
start = source.find("static char *ollama_chat_with_limit")
if start < 0:
    raise SystemExit("FAIL: payload builder was not found")
guard = source.find("ollama_current_user_turn_truncated", start)
serialize = source.find("ollama_json_append_message(arr, role, content, limits[i])", guard)
if guard < 0 or serialize < 0 or guard > serialize:
    raise SystemExit("FAIL: active-turn truncation guard must run before payload serialization")
window = source[max(start, guard - 6000):guard + 1800]
checks = {
    "the exact authoritative query length is checked": "size_t query_length = strlen(authoritative_query);" in window,
    "raw and explicitly wrapped current turns are handled": "BEGIN CURRENT USER MESSAGE" in source[start:guard],
    "serialized message limits verify the entire query": "query_length <= serialized_length - query_offset" in window,
    "inference fails instead of sending a prefix": "return NULL;" in window,
    "diagnostics omit user text": "user_text_logged=no" in window,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("FAIL: " + "; ".join(failed))
print("Active user turn truncation guard smoke passed.")
