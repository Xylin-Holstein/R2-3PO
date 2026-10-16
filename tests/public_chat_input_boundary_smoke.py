#!/usr/bin/env python3
"""Regression guard for every public R2 chat entry point's input boundary."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
start = core.find("char *r2_talk(const char *message)")
end = core.find("\\nint r2_conversation(void)", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate public r2_talk().")
body = core[start:end]

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

validation = body.find("if (!message || !reply_has_visible_text(message))")
size_check = body.find("strnlen(message, (size_t)OLLAMA_MAX_MESSAGE_CHARS + 1U)")
turn_activity = body.find("atomic_store(&last_conversation_activity_ms")
turn_lock = body.find("pthread_mutex_lock(&conversation_turn_lock)")
require(validation >= 0 and size_check > validation,
        "null, empty, and whitespace-only public messages are rejected")
require(size_check >= 0 and "input_chars > OLLAMA_MAX_MESSAGE_CHARS" in body,
        "public API rejects oversized messages without truncation")
require("Nothing was sent or truncated" in body and "no turn was stored" in body,
        "oversized input receives an explicit user-visible rejection and does not enter history")
require(turn_activity > size_check and turn_lock > size_check,
        "invalid input is rejected before turn locks, activity updates, or conversation mutation")
require("r2_talk_impl(message)" in body,
        "valid messages continue through the single shared conversation implementation")
print("Public chat input-boundary regression passed.")
