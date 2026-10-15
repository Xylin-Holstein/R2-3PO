#!/usr/bin/env python3
"""Guard the final retrieval-context budget after all evidence sources are appended."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *chat_with_relevant_memories(")
end = core.find("\n}\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate chat_with_relevant_memories().")
body = core[start:end + 3]

def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

cap = body.find("size_t final_context_limit = explicit_recall ? 28000U : 16000U;")
live_visual = body.find("CURRENT LIVE VISUAL OBSERVATION")
copy_messages = body.find("size_t base_count")
require(cap >= 0, "final assembled context has a recall/focused size cap")
require(live_visual >= 0 and cap > live_visual,
        "cap is applied after the optional live visual section is appended")
require(copy_messages >= 0 and cap < copy_messages,
        "retrieval evidence is bounded before conversation history is copied into the prompt")
require("r2_bound_context_records(memory_context, final_context_limit);" in body,
        "oversized retrieved evidence is clipped at a record boundary")
require("The user's actual message remains" in body,
        "retrieval cap explicitly preserves the user's actual message")
print("Final retrieved-context budget regression passed.")
