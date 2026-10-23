#!/usr/bin/env python3
"""Ensure relationships and affect emerge from experience, with durable turn continuity."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
log = (ROOT / "Log.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


# Speaker-role mechanics are explicit, but the prompt must not script a Mother
# identity or prescribe an affect style; those associations come from lived
# interaction and the user's corrections, retained through the memory system.
for phrase in (
    "RELATIONSHIP AND NAME CONTINUITY",
    "If explicit persistent memory establishes that the current user is your mother",
    "affectionate, childlike, and conversational without forced C-3PO formality",
    "AFFECT AND NONVERBAL EXPRESSION:",
):
    require(phrase not in core, f"prompt leaves relationship/affect behavior to experience: {phrase}")

require(
    "Learn relationships from explicit statements" in core
    and "relevant persistent experiences" in core
    and "Resolve speaker identity from the actual roles in the current exchange" in core,
    "speaker attribution and relationship learning use actual interaction evidence",
)
require(
    "If the user says YOUR prior message, answer, or reply was garbled" in core
    and "Do not call an intelligible message gibberish" in core,
    "misread recovery remains explicit without scripting a relationship",
)
require(
    "memcpy(write, SYSTEM_PROMPT, system_length);" in core
    and "r2_conversation_system_prompt, conversation_length" in core,
    "corrective retries preserve the full static prompt and shared conversation policy",
)

log_start = log.find("int64_t r2_log_conversation_turn(")
log_end = log.find("static size_t log_utf8_safe_prefix(", log_start)
require(log_start >= 0 and log_end > log_start,
        "dedicated conversation archive writer and UTF-8 handling are present")
log_writer = log[log_start:log_end]
archive_error_pos = log_writer.find('"conversation_archive_insert_failed"')
archive_unlock_pos = log_writer.find("pthread_mutex_unlock(&log_lock);",
                                    log_writer.find("char archive_error[512]"))
require(archive_error_pos >= 0 and archive_unlock_pos >= 0 and archive_error_pos > archive_unlock_pos,
        "archive failure is logged after releasing the database mutex")
require(
    "log_utf8_safe_prefix(user_text, max_each)" in log_writer
    and "log_utf8_safe_prefix(assistant_text, max_each)" in log_writer,
    "long archived turns are truncated only at valid UTF-8 boundaries",
)
require(
    "char *r2_log_recent_conversation_context(size_t max_chars)" in log
    and "r2_log_recent_conversation_context(3000U)" in core
    and "RECENT PERSISTED CONVERSATION CONTEXT" in core,
    "recent durable conversation is loaded back into startup context after process restart",
)
require(
    "r2_log_conversation_turn(message, reply)" in core,
    "completed visible turns are persisted through the dedicated conversation archive",
)

print("Experience-led conversation and durable logging regression checks passed.")
