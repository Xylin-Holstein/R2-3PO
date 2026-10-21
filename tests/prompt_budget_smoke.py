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

retry_start = core.find("static const char R2_CORRECTIVE_RETRY_INSTRUCTION[] =")
retry_end = core.find(";\n", retry_start)
if retry_start < 0 or retry_end < 0:
    raise SystemExit("Corrective retry instruction is missing or incomplete")
retry_definition = core[retry_start:retry_end]
retry_literals = re.findall(r'"(?:\\.|[^"\\])*"', retry_definition)
try:
    retry_instruction = "".join(ast.literal_eval(literal) for literal in retry_literals)
except (SyntaxError, ValueError) as exc:
    raise SystemExit(f"Could not decode corrective retry instruction: {exc}")
if len(retry_instruction) > 263:
    raise SystemExit(
        f"Corrective retry instruction has {len(retry_instruction)} characters; "
        "it must fit the smallest tested override budget without truncation"
    )
for required in (
    "prior answer was nonresponsive",
    "Answer the exact user message",
    "resolve speaker roles",
    "quotes are history",
    "Don't invent facts",
    "unintelligible",
):
    if required not in retry_instruction:
        raise SystemExit(f"Corrective retry instruction omits {required!r}")

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
    "compact_messages, compact_count, R2_CORRECTIVE_RETRY_INSTRUCTION,",
    "copy,\n            base_count,\n            NULL,",
    "message_add_system_prompt_sections_locked() != 0",
):
    if required not in core:
        raise SystemExit(f"Full prompt is not preserved across normal/retry paths: {required}")
if "r2_conversation_system_prompt" in core or "r2_retry_system_prompt" in core:
    raise SystemExit("A compact substitute prompt still exists in the source.")

# The context returned by r2_ollama_context_tokens() is a hardware-aware
# ceiling. Oversized estimates must not trigger growth beyond it or collapse
# the reply to 128 tokens; the semantic allocator chooses which evidence fits.
def allocated_context_and_output(required_input_chars: int, requested_output: int, context_ceiling: int) -> tuple[int, int]:
    required_input_tokens = (required_input_chars * 10 + 31) // 32 + 512
    max_predict = max(512, context_ceiling // 4)
    output_tokens = min(requested_output, max_predict)
    # If the estimate exceeds the ceiling, lower-priority evidence is allocated
    # by the serializer; it is not a reason to expand the context or starve reply.
    _estimate_exceeds_ceiling = required_input_tokens > context_ceiling
    return context_ceiling, output_tokens

if allocated_context_and_output(16445 + 1500, 4096, 4096) != (4096, 1024):
    raise SystemExit("A 4k hardware ceiling was exceeded or the reply budget was starved")

if allocated_context_and_output(42000, 4096, 8192) != (8192, 2048):
    raise SystemExit("An 8k hardware ceiling was exceeded or the reply budget was starved")

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
    "size_t reality_context_chars = 0;",
    "size_t retrieved_context_chars = 0;",
    "size_t startup_memory_chars = 0;",
    "size_t learning_context_chars = 0;",
    "size_t reality_to_protect =",
    "size_t retrieval_to_protect =",
    "size_t startup_to_protect =",
    "size_t learning_to_protect =",
    "size_t protected_later = 0;",
    "remaining > protected_later ? remaining - protected_later : 0;",
    "size_t available_for_message =",
    "int max_predict = num_ctx / 4;",
    "if (required_input_tokens > (size_t)num_ctx)",
    "configured ceiling (%d); allocating by semantic priority",
    "configured ceiling (%d); allocating by semantic priority.",











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
