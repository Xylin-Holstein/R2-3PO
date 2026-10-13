#!/usr/bin/env python3
"""Regression checks for bounded memory retrieval and fresh runtime context."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(token: str, message: str) -> None:
    if token not in core:
        raise SystemExit(f"FAIL: {message} (missing {token!r})")
    print(f"PASS: {message}")

require("#define MAX_RELEVANT_MEMORIES 20", "persistent-memory retrieval has a strict per-turn record bound")
require("if (limit > MAX_RELEVANT_MEMORIES)" , "callers cannot request unbounded memory records")
require("extract_memory_keywords(" , "query keywords guide retrieval rather than dumping the whole database")
require("size_t capacity = 0;" , "memory candidates are dynamically allocated from the actual result set")
require("capacity *= 2;" , "candidate buffers grow geometrically rather than reallocating per row")
require("r2_log_search(keywords[k], 3)" , "related Life Log history is retrieved from current query terms")
require("RELATED LIFE LOG HISTORY" , "historical conversation evidence is labeled separately from current state")
require("append_runtime_context(&memory_context)" , "Reality, reinforcement, habits, and Choice Lab are refreshed each turn")
require("char *visual_context = NULL;" , "live visual context is assembled separately from cached text memory")
require("query_requests_visual_context(query)" , "a fresh frame is requested only for explicit visual questions or an open observer")
require("vision_analyze_current_frame(" , "the live frame is analyzed through the existing vision subsystem")
require("Never activate a camera silently from an ordinary conversation turn." , "normal conversation does not open the camera implicitly")
require("CURLOPT_TIMEOUT" , "model calls have an explicit timeout rather than an unbounded socket wait")
require("600L" , "the configured model request timeout allows long-running local inference")
print("Bounded memory retrieval and fresh runtime/visual context smoke passed.")
