#!/usr/bin/env python3
"""Verify R2 keeps its complete instructions and allocates against real context."""
from pathlib import Path
import ast
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def decode_c_string_array(name: str, terminator: str) -> str:
    start = core.find(name)
    end = core.find(terminator, start)
    if start < 0 or end < 0:
        raise SystemExit(f"Could not locate complete C string array: {name}")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', core[start:end])
    try:
        return "".join(ast.literal_eval(literal) for literal in literals)
    except (SyntaxError, ValueError) as exc:
        raise SystemExit(f"Could not decode {name}: {exc}")


system_prompt = decode_c_string_array("static const char *SYSTEM_PROMPT =", ";\n")
conversation_prompt = decode_c_string_array(
    "static const char r2_conversation_system_prompt[] =",
    "\n\nstatic char *r2_build_retry_system_prompt",
)
if len(system_prompt) < 20000:
    raise SystemExit(f"The full SYSTEM_PROMPT appears shortened: {len(system_prompt)} chars")
if len(conversation_prompt) < 6500:
    raise SystemExit(f"The original conversation instructions were shortened: {len(conversation_prompt)} chars")

for required in (
    "IDENTITY AND PERSISTENT MEMORY",
    "REALITY CONTEXT: USE IT, DO NOT RECITE IT",
    "CONVERSATION AND DIARY ARE DIFFERENT",
    "REAL FILE TOOLS",
    "REAL BACKGROUND HANDS",
    "AUTONOMOUS REFLECTION",
    "ALTERNATE-SELF / CHOICE LAB",
    "GENERAL",
):
    if required not in system_prompt:
        raise SystemExit(f"Full SYSTEM_PROMPT is missing section: {required}")

for required in (
    "INTERPRETATION:",
    "SPEAKER AND RELATIONSHIP LEARNING:",
    "REALITY CONTEXT RULES:",
    "AFFECT AND NONVERBAL EXPRESSION:",
    "Never infer that a human message is an internal handoff",
    "If the user says YOUR prior message",
):
    if required not in conversation_prompt:
        raise SystemExit(f"Restored conversation prompt is missing: {required}")

# There must be no application-level override/category/aggregate quota.
for forbidden in (
    "OLLAMA_MAX_SYSTEM_MESSAGE_CHARS",
    "OLLAMA_MAX_SYSTEM_TOTAL_CHARS",
    "OLLAMA_MAX_TOTAL_CHARS",
    "OLLAMA_MAX_MESSAGE_CHARS",
    "r2_compute_system_context_budget(",
    "if (override_length > override_budget)",
    "budget.override_budget",
    "capabilities_used += length",
):
    if forbidden in core:
        raise SystemExit(f"Arbitrary prompt cap remains in the core: {forbidden}")

if "size_t input_budget = (size_t)(num_ctx - num_predict - 512) * 32U / 10U;" not in core:
    raise SystemExit("Inference input budget no longer follows the actual Ollama context.")
if "size_t override_length = system_override ? strlen(system_override) : 0;" not in core:
    raise SystemExit("The complete conversation prompt is not passed through unchanged.")
if "for (size_t i = count; i > 0; --i)" not in core:
    raise SystemExit("History selection still has a fixed recent-turn count cap.")
if "if (length == 0 || length > remaining) continue;" not in core:
    raise SystemExit("Conversation history must be included whole rather than cut mid-message.")
if "if (length <= remaining && length > 0)" not in core:
    raise SystemExit("The active user turn must be included whole rather than cut mid-message.")

def input_budget(num_ctx: int, num_predict: int) -> int:
    return max(0, num_ctx - num_predict - 512) * 32 // 10

for num_ctx, num_predict in ((8192, 1024), (12288, 2048), (16384, 2048)):
    budget = input_budget(num_ctx, num_predict)
    if budget <= 0:
        raise SystemExit(f"Invalid input budget at context={num_ctx}")
    if num_ctx >= 12288 and budget < len(conversation_prompt):
        raise SystemExit(
            f"Full conversation prompt cannot fit its allocated input window at context={num_ctx}: "
            f"{len(conversation_prompt)} > {budget}"
        )

print(
    f"full prompt allocation smoke passed: SYSTEM_PROMPT={len(system_prompt)} chars, "
    f"conversation policy={len(conversation_prompt)} chars; no fixed prompt caps remain"
)
