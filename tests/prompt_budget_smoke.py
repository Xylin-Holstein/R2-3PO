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

# Core identity and recovery rules are part of the full static prompt, not a
# disposable compact override. The normal and retry paths must both send the
# full section set through the same relevance-aware allocator.
for required in (
    "RECOVERY AFTER A MISREAD",
    "treat that as feedback about your reply",
    "Do not reinterpret typed text as audio",
    "Internal tool results are action evidence, not user speech",
    "Use current-turn context and persistent memory",
):
    if required not in prompt:
        raise SystemExit(f"Full R2 prompt omits required conversation/recovery behavior: {required}")

for required in (
    "static int r2_is_static_system_prompt_section(const Message *message)",
    "r2_is_static_system_prompt_section(&messages_copy[i])",
    "Message *compact_messages = calloc(message_count + 2U",
    "compact_messages, compact_count, NULL,",
    "copy,\n            base_count,\n            NULL,",
    "message_add_system_prompt_sections_locked() != 0",
):
    if required not in core:
        raise SystemExit(f"Full prompt is not preserved across normal/retry paths: {required}")
if "r2_conversation_system_prompt" in core or "r2_retry_system_prompt" in core:
    raise SystemExit("A compact substitute prompt still exists in the source.")

# The active instructions are allocated as complete sections. Context/evidence
# may use the remaining budget, but core instructions are not clipped to an
# arbitrary fraction of it.
def required_context_tier(required_input_chars: int, output_tokens: int, current_context: int) -> int:
    required_input_tokens = (required_input_chars * 10 + 31) // 32 + 512
    required_context_tokens = required_input_tokens + output_tokens
    if current_context < required_context_tokens <= 16384:
        tier = ((required_context_tokens + 2047) // 2048) * 2048
        if tier <= 16384:
            return tier
    return current_context

# The complete identity section must not disappear merely because a request
# starts with a 4k context. The allocator must expand before serialization.
if required_context_tier(16445 + 1500, 1024, 4096) != 8192:
    raise SystemExit("A 4k context was not expanded to fit the full prompt and current turn")

def allocated_context_and_output(required_input_chars: int, requested_output: int, current_context: int) -> tuple[int, int]:
    required_input_tokens = (required_input_chars * 10 + 31) // 32 + 512
    current_max = max(512, current_context // 4)
    current_output = min(requested_output, current_max)
    if required_input_tokens <= current_context and current_output <= current_context - required_input_tokens:
        return current_context, current_output
    tier = (current_context // 2048 + 1) * 2048
    while tier <= 16384:
        tier_output = min(requested_output, max(512, tier // 4))
        if required_input_tokens <= tier:
            available_output = tier - required_input_tokens
            if tier_output <= available_output:
                return tier, tier_output
            if tier == 16384:
                return tier, max(128, min(tier_output, available_output))
        tier += 2048
    return current_context, min(current_output, max(128, current_context - required_input_tokens))

if allocated_context_and_output(16445 + 1500, 4096, 4096) != (8192, 2048):
    raise SystemExit("Context growth did not restore the output ceiling allowed by the expanded context")

if allocated_context_and_output(42000, 4096, 4096) != (16384, 2747):
    raise SystemExit("The maximum context tier did not preserve high-priority input when output space is tight")

for required in (
    "static R2SystemContextBudget r2_compute_system_context_budget(",
    "size_t system_budget, size_t override_length",
    "size_t context_budget = system_budget > override_length",
    "budget.reality_reserve = context_budget / 6;",
    "if (budget.reality_reserve > 6000)",
    "budget.retrieval_reserve = context_budget / 8;",
    "budget.capabilities_reserve = context_budget / 6;",
    "budget.learning_reserve = context_budget / 3;",
    "if (budget.learning_reserve > 8192)",
    "budget.learning_reserve = 8192;",
    "r2_compute_system_context_budget(system_budget, override_length)",
    "size_t reality_reserve = context_budgets.reality_reserve;",
    "int is_reality_evidence =",
    "length > reality_reserve",
    "int has_reality_evidence = 0;",
    "int has_retrieved_context = 0;",
    "int has_startup_memory = 0;",
    "int has_learning_context = 0;",
    "size_t protected_later = 0;",
    "remaining > protected_later ? remaining - protected_later : 0;",
    "size_t available_for_message =",
    "int max_predict = num_ctx / 4;",
    "int requested_num_predict = num_predict;",
    "int selected_output = num_predict;",
    "int tier_max_predict = (int)(context_tier / 4U);",
    "int selected_context = num_ctx;",
    "size_t context_tier = ((size_t)num_ctx / 2048U + 1U) * 2048U;",
    "if (required_input_tokens <= context_tier",
    "size_t available_output =",
    "context_tier - required_input_tokens;",
    "if (context_tier < 16384U)",
    "num_ctx = selected_context;",
    "size_t system_budget = input_budget > current_user_reserve",
    "r2_inference_system_priority(",
    "if (priority > 3 && priority != 5 && priority != 6)",
    "Size for core instructions",
    "if (active_user_chars < 1500U)",
    "if ((message_priority <= 2 && !is_reality_evidence) ||",
    "message_priority == 4)",
    "if (message_priority == 3 && length > retrieval_reserve)",
    "if (priority == 3 && length > OLLAMA_MAX_SYSTEM_TOTAL_CHARS / 8U)",
    "return 2;",
    "return r2_query_matches_specific_capability(query) ? 4 : 1;",
    "return 3;",
    "return 5;",
    "return 6;",
    "return 8;",
):
    if required not in core:
        raise SystemExit(f"Instruction-first context allocation is missing: {required}")

# A large live Reality snapshot must not consume the allocations reserved
# for the later retrieval, startup-memory, and learning sources.
def protected_room(system_budget: int, used: int, reserves: tuple[int, ...]) -> int:
    return max(0, system_budget - used - sum(reserves))

budget = 20000
identity_chars = 1910
static_reality_instruction_chars = 3766
reality_reserve = min(budget // 6, 6000)
retrieval_reserve = budget // 8
startup_reserve = min(budget // 10, 1600)
learning_reserve = min(budget // 3, 8192)
future_db_reserves = (retrieval_reserve, startup_reserve, learning_reserve)
if protected_room(
    budget, identity_chars,
    (reality_reserve, *future_db_reserves),
) < static_reality_instruction_chars:
    raise SystemExit("Reality evidence reserves unexpectedly crowd out the whole static action contract")
dynamic_reality_chars = min(20000, reality_reserve)
remaining_after_reality = budget - identity_chars - static_reality_instruction_chars - dynamic_reality_chars
if remaining_after_reality < sum(future_db_reserves):
    raise SystemExit("A large Reality snapshot can still crowd out retrieval/startup/learning reserves")

# The full prompt is split at semantic boundaries so the query-specific section
# is selected by priority instead of being truncated inside one giant message.
for required in (
    "static const char *const section_markers[]",
    "enum { SECTION_COUNT = sizeof(section_markers) / sizeof(section_markers[0]) };",
    'message_add_pinned("system", section)',
    "r2_capability_section_matches_query(content, query)",
    "CURRENT TURN RETRIEVED CONTEXT",
):
    if required not in core:
        raise SystemExit(f"Semantic system-prompt assembly is missing: {required}")

print(
    f"system prompt budget smoke passed: prompt={len(prompt)} chars, "
    f"per-message={per_message}, aggregate-system={system_total}, history={history_total}; "
    "full instruction sections preserved before evidence allocation"
)
