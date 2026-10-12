#!/usr/bin/env python3
"""Guard the model-assisted associative retrieval path without requiring Ollama."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

helper_start = core.find("static char *r2_build_associative_search_query(")
helper_end = core.find("static char *chat_with_relevant_memories(", helper_start)
require(helper_start >= 0 and helper_end > helper_start,
        "an isolated associative query planner exists before normal retrieval")
helper = core[helper_start:helper_end]
require(
    "ollama_chat_with_limit(" in helper
    and "Do NOT answer the user" in helper
    and "natural paraphrases" in helper,
    "the local model interprets message meaning into paraphrase cues rather than relying on a recall trigger phrase",
)
require(
    "original user text remains unchanged" in helper
    and "r2_reply_sanitize(generated)" in helper,
    "generated retrieval cues are treated as search input only and sanitized",
)
builder_start = core.find("static char *chat_with_relevant_memories(")
builder_end = core.find("/* ============================================================\n   STRING HELPERS", builder_start)
builder = core[builder_start:builder_end]
require(
    "r2_build_associative_search_query(query ? query : \"\")" in builder,
    "associative retrieval planning runs on every ordinary conversation turn",
)
require(
    "get_relevant_memories(associative_query, memory_limit)" in builder
    and "extract_memory_keywords(associative_query, keywords)" in builder,
    "generated associations are used to search persistent memory and Life Log history",
)
require(
    "r2_diary_search" in builder and "r2_visual_search" in builder,
    "the same associations also search the separate diary and visual experience stores",
)
require(
    "RELATED DIARY REFLECTIONS" in builder and "RELATED VISUAL EXPERIENCES" in builder,
    "diary and visual evidence is labeled as historical context rather than current fact",
)
require(
    "message_add(\"user\", message)" in core
    and "CURRENT TURN RETRIEVED CONTEXT" in builder,
    "the original user message remains a separate authoritative turn",
)
print("R2 associative retrieval smoke test passed.")
