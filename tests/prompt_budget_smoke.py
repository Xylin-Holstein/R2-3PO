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

# Guard the runtime allocator, not just the size of the static prompt. Under
# constrained context, identity, live Reality, current-turn retrieval, and the
# relevant capability section each get an explicit share.
for required in (
    "size_t max_user_reserve = input_budget > 1500",
    "current_user_reserve > max_user_reserve",
    "static R2SystemContextBudget r2_compute_system_context_budget(",
    "budget.reality_reserve = system_budget / 10;",
    "budget.identity_budget = system_budget / 4;",
    "budget.retrieval_reserve = system_budget / 10;",
    "budget.capabilities_reserve = system_budget / 32;",
    "budget.override_budget = system_budget > reserved",
    "size_t current_user_reserve = 400;",
    "static const char r2_conversation_system_prompt_compact[]",
    "r2_conversation_prompt_for_context()",
    "budget.identity_budget = system_budget / 4;",

    "r2_compute_system_context_budget(system_budget)",
    "message_add_system_prompt_sections_locked() != 0",
    "static const char *const section_markers[]",
    "const char *section_start = starts[i];",
    "enum { SECTION_COUNT = sizeof(section_markers) / sizeof(section_markers[0]) };",
    'message_add_pinned("system", section)',
    "r2_capability_section_matches_query",
    "r2_inference_system_priority(",
    "ollama_authoritative_user_query",
    "capabilities_used += length",
    "Prefer a complete sentence boundary when the cap falls inside the",
    "message_priority == 0 && length > identity_budget",
    "message_priority == 1 && length > reality_reserve",
    "message_priority == 2 && length > retrieval_reserve",
    "message_priority == 3 || message_priority == 4",
    "r2_safe_prompt_prefix_length(content, identity_budget, 0)",
    "r2_safe_prompt_prefix_length(content, reality_reserve, 1)",
    "r2_safe_prompt_prefix_length(content, retrieval_reserve, 1)",
    'json_object_object_add(override_message, "content",',
    'bounded_override) != 0',
    'json_object_put(bounded_override);',
    "override_length = r2_utf8_safe_prefix_length(system_override, override_length);",
):
    if required not in core:
        raise SystemExit(f"Adaptive prompt budget is missing its context safeguard: {required}")

# The sentence-boundary search can fall back to an arbitrary byte offset.
# Confirm that the final system-override cap is re-aligned before JSON creation.
override_clip_start = core.find("if (override_length > override_budget)")
override_clip_end = core.find("size_t total_chars = override_length;", override_clip_start)
if override_clip_start < 0 or override_clip_end < 0:
    raise SystemExit("Could not isolate the system-override clipping path")
override_clip = core[override_clip_start:override_clip_end]
boundary_choice = override_clip.find("override_length = boundary >=")
utf8_alignment = override_clip.find(
    "override_length = r2_utf8_safe_prefix_length(system_override, override_length);"
)
if boundary_choice < 0 or utf8_alignment <= boundary_choice:
    raise SystemExit("System-override clipping can split a multibyte UTF-8 character")

def allocation_for_context(num_ctx: int, num_predict: int, override_length: int,
                           current_user_reserve: int = 400) -> tuple[int, int, int, int, int, int, int, int, int]:
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
    reality_reserve = min(system_budget // 10, 3000)
    identity_minimum = system_budget // 4
    if identity_minimum < 500 and system_budget > reality_reserve + 1000:
        identity_minimum = 500
    retrieval_reserve = system_budget // 10
    capabilities_reserve = system_budget // 32
    identity_budget = min(identity_minimum, system_budget - reality_reserve)
    reserved = (reality_reserve + identity_budget + retrieval_reserve
                + capabilities_reserve)
    override_budget = (
        system_budget - reserved if system_budget > reserved
        else system_budget // 2
    )
    bounded_override = min(override_length, override_budget)
    return (system_budget, bounded_override, identity_budget, reality_reserve,
            retrieval_reserve, capabilities_reserve, current_user_reserve,
            input_budget, identity_minimum)

# The compact retry must fit intact in the smallest supported context; otherwise
# sentence-boundary clipping would silently remove the corrective instruction.
compact_match = re.search(
    r'static const char r2_conversation_system_prompt_compact\[\] =\s*("(?:\\.|[^"\\])*");',
    core,
)
retry_match = re.search(
    r'static const char compact_retry_instruction\[\] =\s*("(?:\\.|[^"\\])*");',
    core,
)
if not compact_match or not retry_match:
    raise SystemExit("Could not locate compact conversation/retry prompt literals")
compact_length = len(ast.literal_eval(compact_match.group(1)))
retry_length = len(ast.literal_eval(retry_match.group(1)))
retry_override_length = compact_length + retry_length + 2
_, bounded_retry_override, *_ = allocation_for_context(
    2048, 512, retry_override_length, 400
)
if bounded_retry_override < retry_override_length:
    raise SystemExit(
        f"Compact corrective retry is clipped at 2048 context: "
        f"needs {retry_override_length}, receives {bounded_retry_override}"
    )

# Exercise low-memory, normal, and long-user-message paths. The allocator must
# leave a system budget and a bounded current-turn reservation at every size.
for num_ctx, num_predict, override_length, requested_user_reserve in (
    (2048, 384, 12000, 400),
    (2048, 384, 12000, 12000),
    (4096, 384, 12000, 12000),
    (8192, 768, 12000, 1500),
    (16384, 3072, 12000, 12000),
):
    system_budget, override, identity, reality, retrieval, capabilities, user_reserve, input_budget, identity_minimum = (
        allocation_for_context(num_ctx, num_predict, override_length,
                              requested_user_reserve)
    )
    if system_budget <= 0 or override > system_budget:
        raise SystemExit(f"Invalid override allocation at context={num_ctx}")
    if identity < identity_minimum or reality <= 0 or retrieval <= 0 or capabilities <= 0:
        raise SystemExit(
            f"Identity/Reality/retrieval/capability context starved at context={num_ctx}: "
            f"system={system_budget}, override={override}, identity={identity}, "
            f"reality={reality}, retrieval={retrieval}, capabilities={capabilities}"
        )
    if system_budget > 3000 and override < system_budget // 6:
        raise SystemExit(
            f"Conversation contract received too little budget at context={num_ctx}: "
            f"system={system_budget}, override={override}"
        )
    if override + identity + reality + retrieval + capabilities > system_budget:
        raise SystemExit(f"Prompt allocations exceed system budget at context={num_ctx}")
    if system_budget - (override + identity + reality) < retrieval + capabilities:
        raise SystemExit(f"Retrieved memory/capability shares were consumed by higher-priority blocks at context={num_ctx}")
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
