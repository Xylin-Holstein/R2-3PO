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

budget_start = core.find("static R2SystemContextBudget r2_compute_system_context_budget(")
budget_end = core.find("\n}\n", budget_start)
if budget_start < 0 or budget_end < 0:
    raise SystemExit("Could not locate the prompt-budget allocator.")
budget = core[budget_start:budget_end + 3]
require("budget.retrieval_reserve = system_budget / 6;" in budget,
        "the serializer explicitly reserves a share for current-turn retrieval")
require("budget.capabilities_reserve = system_budget / 12;" in budget
require("if (system_budget < 18000)" in core and "budget.capabilities_reserve = system_budget / 20;" in core,
        "long turns switch to a conversation-first prompt allocation policy"),
        "the serializer reserves space for capability instructions after active-turn evidence")
require("r2_compute_system_context_budget(system_budget)" in core,
        "the actual Ollama serializer uses the tested prompt-budget allocator")
require("message_add_system_prompt_sections_locked() != 0" in core,
        "the static identity and capability prompts are split before runtime budgeting")
require("capability_context=%d" in core and "audit_capability_context = 1" in core,
        "prompt audit reports whether capability instructions survived serialization")
require("length > reality_reserve" in core,
        "live Reality cannot consume more than its allocated prompt share")
require("The actual user" in body and "message is stored separately" in body,
        "retrieval cap explicitly preserves the user's actual message")
print("Final retrieved-context budget regression passed.")
