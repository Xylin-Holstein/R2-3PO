#!/usr/bin/env python3
"""Guard that the latest core refreshes live state and retrieves continuity per turn."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
start = core.find("static char *chat_with_relevant_memories(")
end = core.find("/* ============================================================\n   STRING HELPERS", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate the current context builder.")
builder = core[start:end]

def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require("r2_reality_context()" in builder, "live Reality is fetched for each current turn")
require("r2_log_search(keywords[k], 3)" in builder, "historical continuity is searched from current-turn keywords")
require("append_context_block(&memory_context" in builder, "current state is composed with retrieved memories additively")
require("r2_reward_context()" in builder and "r2_addiction_report()" in builder,
        "learned reward and habit evidence can join the current context")
require("r2_altself_list(5)" in builder, "hypothetical state is included only for relevant questions")
require("context_cache_get(" not in core and "R2_CONTEXT_CACHE_ENTRIES" not in core,
        "the latest core does not reuse stale cached Reality or memory evidence")
print("Live current-turn context refresh smoke passed.")
