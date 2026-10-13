#!/usr/bin/env python3
"""Guard R2's full core prompt and bounded Ollama context limits."""
from pathlib import Path
import ast
import re

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def macro(name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", core, re.MULTILINE)
    if not match:
        raise SystemExit(f"Missing numeric limit: {name}")
    return int(match.group(1))


per_message = macro("OLLAMA_MAX_SYSTEM_MESSAGE_CHARS")
system_total = macro("OLLAMA_MAX_SYSTEM_TOTAL_CHARS")
history_total = macro("OLLAMA_MAX_TOTAL_CHARS")
if per_message > 24000 or per_message < 23000:
    raise SystemExit(f"System message cap must preserve the complete pinned prompt: {per_message}")
if system_total > 42000 or system_total < 40000:
    raise SystemExit(f"Aggregate system context cap must preserve identity and live context: {system_total}")
if history_total > 48000 or history_total < 46000:
    raise SystemExit(f"Bounded total history cap should remain finite but preserve the full prompt: {history_total}")

start = core.find("static const char *SYSTEM_PROMPT =")
end = core.find(";\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the complete SYSTEM_PROMPT definition")
definition = core[start:end]
literals = re.findall(r'"(?:\\.|[^"\\])*"', definition)
try:
    prompt = "".join(ast.literal_eval(literal) for literal in literals)
except (SyntaxError, ValueError) as exc:
    raise SystemExit(f"Could not decode SYSTEM_PROMPT string literals: {exc}")
if not prompt:
    raise SystemExit("SYSTEM_PROMPT is empty")
if len(prompt) > per_message:
    raise SystemExit(
        f"SYSTEM_PROMPT has {len(prompt)} characters, exceeding its {per_message}-character cap"
    )
for required in (
    "CONVERSATION AND DIARY ARE DIFFERENT",
    "REAL FILE TOOLS",
    "GENERAL",
):
    if required not in prompt:
        raise SystemExit(f"Complete prompt is missing its expected trailing section: {required}")

# Guard the runtime allocator, not just the size of the static prompt. When
# available RAM lowers num_ctx, a long per-call override must not consume the
# whole system budget and crowd out the core identity/live Reality context.
for required in (
    "size_t max_user_reserve = input_budget > 1500",
    "current_user_reserve > max_user_reserve",
    "size_t reality_reserve = system_budget / 3;",
    "size_t override_budget = system_budget / 4;",
    "size_t identity_budget = system_budget > override_length + reality_reserve",
    "message_priority == 0 && length > identity_budget",
    'json_object_object_add(override_message, "content", bounded_override)',
):
    if required not in core:
        raise SystemExit(f"Adaptive prompt budget is missing its context safeguard: {required}")

def allocation_for_context(num_ctx: int, num_predict: int, override_length: int,
                           current_user_reserve: int = 1500) -> tuple[int, int, int, int, int, int]:
    input_budget = (num_ctx - num_predict - 512) * 32 // 10
    input_budget = min(input_budget, history_total)
    max_user_reserve = (
        input_budget - 1500 if input_budget > 1500 else input_budget // 2
    )
    current_user_reserve = min(current_user_reserve, max_user_reserve)
    system_budget = (
        input_budget - current_user_reserve - 500
        if input_budget > current_user_reserve + 500 else 0
    )
    system_budget = min(system_budget, system_total, input_budget)
    bounded_override = min(override_length, system_budget // 4)
    reality_reserve = min(system_budget // 3, 3000)
    identity_budget = max(0, system_budget - bounded_override - reality_reserve)
    return (system_budget, bounded_override, identity_budget, reality_reserve,
            current_user_reserve, input_budget)

# Exercise low-memory, normal, and long-user-message paths. The allocator must
# leave a system budget and a bounded current-turn reservation at every size.
for num_ctx, num_predict, override_length, requested_user_reserve in (
    (2048, 384, 12000, 1500),
    (2048, 384, 12000, 12000),
    (4096, 384, 12000, 12000),
    (8192, 768, 12000, 1500),
    (16384, 3072, 12000, 12000),
):
    system_budget, override, identity, reality, user_reserve, input_budget = (
        allocation_for_context(num_ctx, num_predict, override_length,
                              requested_user_reserve)
    )
    if system_budget <= 0 or override > system_budget // 4:
        raise SystemExit(f"Invalid override allocation at context={num_ctx}")
    if identity <= 0 or reality <= 0:
        raise SystemExit(
            f"Identity/live Reality context starved at context={num_ctx}: "
            f"system={system_budget}, override={override}, "
            f"identity={identity}, reality={reality}"
        )
    if override + identity + reality > system_budget:
        raise SystemExit(f"Prompt allocations exceed system budget at context={num_ctx}")
    if system_budget + user_reserve + 500 > input_budget:
        raise SystemExit(f"Current user reservation exceeds input budget at context={num_ctx}")
    if requested_user_reserve > 1500 and input_budget > 1500 and user_reserve > input_budget - 1500:
        raise SystemExit(f"Long current turn left too little system context at context={num_ctx}")
    if input_budget > 1500 and system_budget < 1000:
        raise SystemExit(f"Long current turn starved system context at context={num_ctx}")

print(
    f"system prompt budget smoke passed: prompt={len(prompt)} chars, "
    f"per-message={per_message}, aggregate-system={system_total}, history={history_total}"
)
