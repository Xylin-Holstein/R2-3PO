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
for required in (
    "static R2SystemContextBudget r2_compute_system_context_budget(",
    "size_t system_budget, size_t override_length",
    "size_t context_budget = system_budget > override_length",
    "budget.capabilities_reserve = context_budget;",
    "r2_compute_system_context_budget(system_budget, override_length)",
    "int max_predict = num_ctx / 4;",
    "size_t system_budget = input_budget > current_user_reserve",
    "if (message_priority <= 2 || message_priority == 4)",
    "if (message_priority == 3 && length > retrieval_reserve)",
    "message_priority == 2",
    "return r2_query_matches_specific_capability(query) ? 4 : 1;",
    "return 3;",
    "return 5;",
    "return 6;",
    "return 8;",
):
    if required not in core:
        raise SystemExit(f"Instruction-first context allocation is missing: {required}")

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
