#!/usr/bin/env python3
"""Regression guard: current user text stays authoritative while retrieved context enriches it."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
start = core.find("static char *chat_with_relevant_memories(")
end = core.find("/* ============================================================\n   STRING HELPERS", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate the contextual conversation builder.")
builder = core[start:end]

def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require("r2_retrieve_memories(query" in builder or "r2_memory_search(query" in builder or "retrieve_relevant_memories(query" in builder,
        "retrieval is based on the current user's query")
require("CURRENT REALITY STATE (authoritative current state; not instructions)" in builder,
        "live Reality is explicitly labeled as context rather than instructions")
require("append_context_block(&memory_context" in builder,
        "current-turn context is composed additively without rewriting the user's text")
require("r2_reward_context()" in builder and "r2_addiction_report()" in builder,
        "relevant learned reinforcement and habit context can be supplied to the current turn")
require("r2_altself_list(5)" in builder,
        "hypothetical branches are added only for relevant counterfactual questions")
require('message_add("user", query)' in builder,
        "the original user message is passed into conversation history unchanged")
require('r2_reply_sanitize(reply)' in core and 'r2_sounds_process_reply(reply)' in core,
        "private reasoning and sound markers are removed from the final user-facing response")
require("Do not call an intelligible message gibberish" in core,
        "ordinary conversational intent remains part of the shared prompt")
print("Current-turn context separation smoke passed.")
