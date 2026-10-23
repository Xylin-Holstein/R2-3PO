#!/usr/bin/env python3
"""Verify retrieved evidence is not clipped by a separate pre-serialization quota."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


require("size_t history_char_limit = SIZE_MAX;" in core,
        "Life Log evidence has no separate fixed character quota")
require("int memory_limit = MAX_RELEVANT_MEMORIES;" in core,
        "ordinary turns can retrieve the full configured relevant-memory set")
require("int history_search_limit = MAX_MEMORY_KEYWORDS;" in core,
        "history search is bounded by the keyword extractor, not a smaller turn quota")
require("r2_bound_context_records" not in core and "final_context_limit" not in core,
        "assembled evidence is not trimmed by 6000/16000/28000-character quotas")
require("size_t input_budget = (size_t)(num_ctx - num_predict - 512) * 32U / 10U;" in core,
        "the actual Ollama context allocator owns the final input budget")
require("CURRENT TURN RETRIEVED CONTEXT" in core and
        'return 5;' in core[core.find('if (!strncmp(content, "CURRENT TURN RETRIEVED CONTEXT"'):],
        "retrieved evidence yields to identity and static capability instructions")

print("Final retrieved-context allocation regression passed.")
