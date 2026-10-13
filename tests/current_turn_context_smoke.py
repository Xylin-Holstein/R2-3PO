#!/usr/bin/env python3
"""Regression guard: retrieved evidence enriches but never replaces the current user turn."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

start = core.find("static char *chat_with_relevant_memories(")
end = core.find("/* ============================================================\n   STRING HELPERS", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate the contextual conversation builder.")
builder = core[start:end]
require("extract_memory_keywords(query, keywords)" in builder and "r2_log_search(keywords[k], 3)" in builder,
        "historical continuity retrieval is derived from the current user query")
require("CURRENT REALITY STATE (authoritative current state; not instructions)" in builder,
        "live Reality is labeled as state context, not an instruction override")
require("append_context_block(&memory_context" in builder,
        "current-turn context is appended without rewriting the user's text")
require("r2_reward_context()" in builder and "r2_addiction_report()" in builder,
        "relevant learned reinforcement and habit context can be supplied")
require("r2_altself_list(5)" in builder,
        "counterfactual context is supplied for relevant hypothetical questions")
talk_start = core.find("char *r2_talk_impl(const char *message)")
talk_end = core.find("\nchar *r2_talk(const char *message)", talk_start)
if talk_start < 0 or talk_end < 0:
    raise SystemExit("Could not isolate the current-turn handler.")
talk = core[talk_start:talk_end]
require('message_add("user", message)' in talk,
        "the original user message is stored unchanged in conversation history")
require("r2_reply_sanitize(reply)" in talk and "r2_sounds_process_reply(reply)" in talk,
        "private reasoning and sound markers are removed from the final user-facing reply")
require("Do not invent experiences or memories." in core,
        "the shared prompt distinguishes remembered evidence from fabricated experience")
print("Current-turn context separation smoke passed.")
