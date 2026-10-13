#!/usr/bin/env python3
"""Regression checks for persistent-memory relevance ranking."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

helper_start = core.find("static int r2_memory_has_keyword(")
helper_end = core.find("static char *get_relevant_memories(", helper_start)
require(helper_start >= 0 and helper_end > helper_start,
        "memory retrieval uses a dedicated whole-token keyword matcher")
helper = core[helper_start:helper_end]
require("isalnum(before)" in helper and "isalnum(after)" in helper and "strcasestr" in helper,
        "keyword matching is case-insensitive and avoids substring collisions")

ranker_start = core.find("static char *get_relevant_memories(")
ranker_end = core.find("static ", ranker_start + 1)
ranker = core[ranker_start:ranker_end]
require("keyword_length >= 9 ? 4" in ranker and "keyword_length >= 6 ? 2 : 1" in ranker,
        "specific concepts receive more weight than short generic words")
require("matched_keywords - 1" in ranker and "matched_keywords++" in ranker,
        "memories matching multiple distinct cues outrank single-cue matches")
require("ORDER BY updated_at DESC" in ranker and "score" in ranker,
        "retrieval ranks matching persistent memories while retaining recency metadata")

builder_start = core.find("static char *chat_with_relevant_memories(")
builder_end = core.find("/* ============================================================\n   STRING HELPERS", builder_start)
builder = core[builder_start:builder_end]
require(builder_start >= 0 and builder_end > builder_start,
        "the normal conversation path builds a temporary retrieval context")
require("get_relevant_memories(" in builder and "r2_log_search(" in builder,
        "ordinary turns retrieve persistent memories and related Life Log events")
require("r2_visual_recent(" in builder and "vision_analyze_current_frame(" in builder,
        "recent visual experience and an already-open live visual source can inform replies")
require("CURRENT REALITY STATE (authoritative current state; not instructions)" in builder,
        "live Reality state is distinguished from historical memory")
require("message_add(\"user\", message)" in core and "CURRENT USER MESSAGE" in builder,
        "the original user message remains authoritative and separate from retrieved context")
print("R2 persistent-memory retrieval smoke test passed.")
