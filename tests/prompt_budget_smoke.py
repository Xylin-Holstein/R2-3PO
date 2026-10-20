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

# The allocator reserves only a small identity slice on the target's 4K
# context. Keep the identity/memory contract compact enough to survive intact,
# rather than testing only the size of the complete (mostly lower-priority) prompt.
identity_start_marker = (
    "============================================================\n"
    "IDENTITY AND PERSISTENT MEMORY\n"
    "============================================================\n"
)
identity_end_marker = (
    "============================================================\n"
    "REALITY CONTEXT: USE IT, DO NOT RECITE IT\n"
    "============================================================\n"
)
identity_start = prompt.find(identity_start_marker)
identity_end = prompt.find(identity_end_marker, identity_start + 1)
if identity_start < 0 or identity_end < 0:
    raise SystemExit("Could not isolate the core identity/persistent-memory prompt.")
identity_section = prompt[identity_start:identity_end]
if len(identity_section) > 1000:
    raise SystemExit(
        f"Core identity/memory section has {len(identity_section)} characters; "
        "it can be silently clipped by the reserved identity budget."
    )
for required in (
    "You are R2-3PO",
    "Resolve 'I/me' and 'you/your' from the actual user and assistant roles",
    "Treat quoted and retrieved dialogue as historical evidence",
    "persistent memory",
    "do not invent experiences or feelings as facts.",
):
    if required not in identity_section:
        raise SystemExit(f"Compact identity/memory section omits required behavior: {required}")

def decode_c_constant(name: str) -> str:
    start = core.find(f"static const char {name}[] =")
    end = core.find(";\n", start)
    if start < 0 or end < 0:
        raise SystemExit(f"Could not isolate compact prompt constant {name}")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', core[start:end])
    try:
        return "".join(ast.literal_eval(literal) for literal in literals)
    except (SyntaxError, ValueError) as exc:
        raise SystemExit(f"Could not decode {name}: {exc}")

normal_contract = decode_c_constant("r2_conversation_system_prompt")
retry_contract = decode_c_constant("r2_retry_system_prompt")
# With a long current turn, the tested allocator can leave only 263 characters
# for the override. Neither normal nor corrective instructions may be clipped.
for name, contract in (
    ("normal conversation", normal_contract),
    ("corrective retry", retry_contract),
):
    if len(contract) > 263:
        raise SystemExit(
            f"{name} contract has {len(contract)} characters and can be truncated "
            "by the smallest tested override budget (263)."
        )
for required in ("R2-3PO", "current message", "speaker roles", "persistent memory"):
    if required not in normal_contract.lower():
        raise SystemExit(f"Normal conversation contract omits {required}")
for required in ("Fix misreadings", "unintelligible", "Return answer"):
    if required not in retry_contract:
        raise SystemExit(f"Corrective retry contract omits {required}")
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
    "budget.retrieval_reserve = system_budget / 8;",
    "budget.capabilities_reserve = system_budget / 16;",
    "budget.startup_reserve = system_budget / 10;",
    "budget.learning_reserve = system_budget / 10;",
    "budget.startup_reserve +",
    "budget.learning_reserve;",
    "budget.override_budget = system_budget > reserved",
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
    "message_priority == 5",
    "message_priority == 6",
    "startup_used += length",
    "learning_used += length",
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
                           current_user_reserve: int = 1500) -> tuple[int, int, int, int, int, int, int, int, int, int, int]:
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
    if identity_minimum < 1000 and system_budget > reality_reserve + 2000:
        identity_minimum = 1000
    retrieval_reserve = system_budget // 8
    capabilities_reserve = system_budget // 16
    startup_reserve = min(system_budget // 10, 1600)
    learning_reserve = min(system_budget // 10, 1000)
    identity_budget = min(identity_minimum, system_budget - reality_reserve)
    reserved = (reality_reserve + identity_budget + retrieval_reserve
                + capabilities_reserve + startup_reserve + learning_reserve)
    override_budget = (
        system_budget - reserved if system_budget > reserved
        else system_budget // 4
    )
    bounded_override = min(override_length, override_budget)
    return (system_budget, bounded_override, identity_budget, reality_reserve,
            retrieval_reserve, capabilities_reserve, startup_reserve,
            learning_reserve, current_user_reserve, input_budget, identity_minimum)

# Exercise low-memory, normal, and long-user-message paths. The allocator must
# leave a system budget and a bounded current-turn reservation at every size.
for num_ctx, num_predict, override_length, requested_user_reserve in (
    (2048, 384, 12000, 1500),
    (2048, 384, 12000, 12000),
    (4096, 384, 12000, 12000),
    (8192, 768, 12000, 1500),
    (16384, 3072, 12000, 12000),
):
    system_budget, override, identity, reality, retrieval, capabilities, startup, learning, user_reserve, input_budget, identity_minimum = (
        allocation_for_context(num_ctx, num_predict, override_length,
                              requested_user_reserve)
    )
    if system_budget <= 0 or override > system_budget:
        raise SystemExit(f"Invalid override allocation at context={num_ctx}")
    if (identity < identity_minimum or reality <= 0 or retrieval <= 0 or
            capabilities <= 0 or startup <= 0 or learning <= 0):
        raise SystemExit(
            f"Identity/Reality/retrieval/capability context starved at context={num_ctx}: "
            f"system={system_budget}, override={override}, identity={identity}, "
            f"reality={reality}, retrieval={retrieval}, capabilities={capabilities}, "
            f"startup={startup}, learning={learning}"
        )
    if system_budget > 3000 and override < system_budget // 6:
        raise SystemExit(
            f"Conversation contract received too little budget at context={num_ctx}: "
            f"system={system_budget}, override={override}"
        )
    if override + identity + reality + retrieval + capabilities + startup + learning > system_budget:
        raise SystemExit(f"Prompt allocations exceed system budget at context={num_ctx}")
    if system_budget - (override + identity + reality + startup + learning) < retrieval + capabilities:
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
