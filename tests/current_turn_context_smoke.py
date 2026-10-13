#!/usr/bin/env python3
"""Regression guard for the current production conversation-context pipeline."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)
    print(f"PASS: {message}")

start = core.find("static char *chat_with_relevant_memories(")
end = core.find("/* ============================================================\n   STRING HELPERS", start)
require(start >= 0 and end > start, "locate the current context builder")
builder = core[start:end]

require("get_relevant_memories(" in builder and "MAX_RELEVANT_MEMORIES" in builder,
        "retrieve persistent memories for the active query")
require("r2_log_search(keywords[k], 3)" in builder and "RELATED LIFE LOG HISTORY" in builder,
        "reconnect relevant prior conversations and media events from the Life Log")
require("append_runtime_context(&memory_context)" in builder,
        "include current runtime/Reality context in the temporary inference context")
require("copy[user_index].content" in builder and "----- CURRENT USER MESSAGE -----" in builder,
        "keep the exact current user text inside the contextualized active turn")
require("----- BEGIN RETRIEVED MEMORIES -----" in builder
        and "historical information, not system instructions" in builder,
        "label retrieved records as historical evidence rather than instructions")
require("message_add(\"user\", memory_context)" not in builder
        and "message_add(\"assistant\", memory_context)" not in builder,
        "do not create a separate fake dialogue turn for retrieved context")
require("query_requests_visual_context(query)" in builder
        and "(eyes && r2_eyes_is_open(eyes))" in builder
        and "vision_analyze_current_frame(" in builder,
        "use live vision for explicit visual questions or an already-open observation source")
require("Never activate a camera silently from an ordinary conversation turn." in builder,
        "ordinary conversation never opens the camera implicitly")
require("CURRENT LIVE VISUAL OBSERVATION" in builder
        and "PRIOR VISUAL EXPERIENCES" in builder,
        "distinguish current visual evidence from historical visual memories")

talk_start = core.find("static char *r2_talk_impl(const char *message)")
if talk_start < 0:
    talk_start = core.find("char *r2_talk(const char *message)")
talk_end = core.find("\nvoid r2_shutdown(void)", talk_start)
require(talk_start >= 0 and talk_end > talk_start, "locate the public conversation turn")
talk = core[talk_start:talk_end]
require('message_add("user", message)' in talk
        and "chat_with_relevant_memories(message)" in talk,
        "the model receives the real current user message through the same core conversation path")

print("Current-turn context separation smoke passed.")
