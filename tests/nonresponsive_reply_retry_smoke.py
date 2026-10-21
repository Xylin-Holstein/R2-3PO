#!/usr/bin/env python3
"""Test the bounded recovery path for nonresponsive typed-text interpretations."""
from pathlib import Path
import ast
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = core.find("static int r2_reply_reverses_speaker_roles(")
end = core.find("static int r2_reply_misclassifies_plain_text(", start)
require(start >= 0 and end > start, "speaker-role reversal guard exists")
role_guard = core[start:end]
for phrase, message in (
    ('"hello r2"', "guard recognizes a greeting directed to R2"),
    ('"hi r2"', "guard recognizes short greetings directed to R2"),
    ('"hey r2"', "guard recognizes casual greetings directed to R2"),
    ('"write a dialogue"', "guard avoids triggering on explicit dialogue-writing requests"),
    ("r2_reply_reverses_speaker_roles(query, reply)", "primary nonresponsive-reply guard invokes speaker-role validation"),
):
    source = role_guard if phrase != "r2_reply_reverses_speaker_roles(query, reply)" else core[end:end+500]
    require(phrase in source, message)

require(
    "A greeting from the user is not a request to rewrite that greeting" in core
    and "do not generate a message from their perspective or greet yourself as R2" in core,
    "main prompt explicitly prevents greeting/identity role reversal",
)
require(
    "never speak as that person or greet yourself as R2" in core,
    "corrective retry explicitly restores R2's speaker identity",
)

start = core.find("static int r2_reply_misclassifies_plain_text(")
end = core.find("\nstatic char *r2_retry_direct_answer(", start)
require(start >= 0 and end > start, "plain-text misclassification guard exists")
guard = core[start:end]
failure_start = core.find("static int r2_reply_contains_known_failure_phrase(")
failure_end = core.find("\nstatic int r2_reply_misclassifies_plain_text(", failure_start)
require(failure_start >= 0 and failure_end > failure_start,
        "known failure-phrase helper exists")
failure_guard = core[failure_start:failure_end]
for phrase in (
    '"jumbled set of sounds"',
    '"appears to be gibberish"',
    '"unable to understand your message"',
    "\"that's a bit garbled\"",
    '"that’s a bit garbled"',
):
    require(phrase in failure_guard, f"failure-phrase helper checks {phrase}")
require(
    "r2_reply_contains_known_failure_phrase(reply)" in guard
    and guard.index("r2_reply_contains_known_failure_phrase(reply)")
        < guard.index("if (!reports_r2_output_problem && strlen(query) < 24)"),
    "explicit failure phrases are checked before short-query guards",
)
for phrase in (
    "if (!reports_r2_output_problem && words < 5)",
    "Keep legitimate audio/transcription discussions out of this guard",
    "reports_r2_output_problem",
    "\"your message\"",
    "\"garbled\"",
    "\"the message is\"",
    "\"you're saying\"",
    "acknowledgement_terms",
    "if (!reports_r2_output_problem && strlen(query) < 24)",
    "if (!reports_r2_output_problem && words < 5)",
    "If the user reports that YOUR prior message",
    "proposes_user_intent",
    "asks_for_confirmation",
    "interpretation_requests",
):
    require(phrase in guard, f"guard checks {phrase}")

start = core.find("static char *r2_retry_direct_answer(")
end = core.find("\nstatic char *chat_with_relevant_memories(", start)
require(start >= 0 and end > start, "corrective retry helper exists")
retry = core[start:end]
for phrase, message in (
    ("messages_copy[index].content,", "retry locates the actual user turn before constructing the compact prompt"),
    ("r2_build_retry_system_prompt()", "retry obtains the complete combined system prompt"),
    ("Message compact_messages[4];", "retry reserves a bounded compact window for context and the exact active turn"),
    ("compact_count < 3", "retry can preserve up to three relevant system contexts without replaying stale dialogue"),
    ("r2_is_retry_context_message(&messages_copy[i])", "retry delegates context selection to the tested relevance predicate"),
    ("compact_messages[compact_count++] = (Message)", "retry appends the exact current user message as the authoritative turn"),
    ("ollama_authoritative_user_query = query;", "retry preserves active-turn identity"),
    ("conversation_reply_token_budget(query)", "retry respects the normal response budget"),
    ("ollama_chat_with_limit(\n        compact_messages, compact_count, retry_system_prompt", "retry uses the compact prompt and does not inject a system message into dialogue"),
):
    require(phrase in retry, message)

builder_start = core.find("static char *r2_build_retry_system_prompt(void)")
builder_end = core.find("\nstatic int r2_is_retry_context_message(", builder_start)
context_start = core.find("static int r2_is_retry_context_message(")
context_end = core.find("\nstatic char *r2_retry_direct_answer(", context_start)
require(context_start >= 0 and context_end > context_start,
        "retry context selector exists")
retry_context = core[context_start:context_end]
for phrase, message in (
    ("PERSISTENT REALITY CONTEXT", "retry context selector retains live Reality"),
    ("CURRENT TURN RETRIEVED CONTEXT", "retry context selector retains focused active-turn evidence"),
    ("R2 PERSISTENT MEMORY - STARTUP CONTEXT", "retry context selector retains broad persistent relationship memory"),
):
    require(phrase in retry_context, message)

require(builder_start >= 0 and builder_end > builder_start,
        "combined corrective system-prompt builder exists")
builder = core[builder_start:builder_end]
for phrase, message in (
    ("RESPONSE CORRECTION:", "builder includes a direct-answer correction instruction"),
    ("Preserve speaker identity and who said what", "builder preserves attribution while correcting a nonresponsive answer"),
    ("attributes words to them", "builder forbids invented attributions"),
    ("that they did not say", "builder forbids adding words the user never said"),
    ("static const char retry_system_prompt[] =", "builder uses a compact self-contained policy that fits the actual inference budget"),
    ("return xstrdup(retry_system_prompt);", "builder returns the compact retry policy without prepending an oversized static identity block"),
    ("RELATIONSHIP AND NAME CONTINUITY:", "compact retry retains learned relationship continuity"),
    ("REPLY STYLE:", "compact retry retains direct-answer style guidance"),
    ("REALITY CONTEXT RULES:", "compact retry retains Reality-use guidance"),
):
    require(phrase in builder, message)

# The serializer has only about 2,193 bytes for its system override at the
# target 4096-token context and a normal-sized user turn. Decode adjacent C
# string literals and check both foreground and corrective policies against
# that real allocation, not just the much larger per-message hard limit.
def c_string_array_length(name: str, end_marker: str) -> int:
    definition = core.find(f"static const char {name}[] =")
    if definition < 0:
        raise SystemExit(f"Could not locate {name}")
    end = core.find(end_marker, definition)
    if end < 0:
        raise SystemExit(f"Could not isolate {name}")
    literals = []
    for line in core[definition:end].splitlines():
        literal = line.strip()
        if literal.startswith('"') and (literal.endswith('"') or literal.endswith('";')):
            if literal.endswith(';'):
                literal = literal[:-1]
            try:
                literals.append(ast.literal_eval(literal))
            except (SyntaxError, ValueError) as exc:
                raise SystemExit(f"Could not decode {name}: {exc}") from exc
    if not literals:
        raise SystemExit(f"Could not decode string literals for {name}")
    return len("".join(literals))

main_prompt_chars = c_string_array_length(
    "r2_conversation_system_prompt", "\n\nstatic char *r2_build_retry_system_prompt(void)"
)
retry_prompt_chars = c_string_array_length(
    "retry_system_prompt", "return xstrdup(retry_system_prompt);"
)
require(main_prompt_chars <= 2193,
        f"foreground policy fits the baseline 4096-context override budget ({main_prompt_chars} chars)")
require(retry_prompt_chars <= 2193,
        f"corrective policy and correction fit the baseline 4096-context override budget ({retry_prompt_chars} chars)")

require(
    "if (retry_after_sanitize_failure ||" in core,
    "empty sanitized replies also receive one corrective retry",
)
require(
    'r2_reply_misclassifies_plain_text(query, reply ? reply : "")' in core,
    "recognized text misclassification triggers the retry",
)
require(
    "static char *r2_validate_corrective_reply(" not in core,
    "corrective model output is not gated by a response classifier or replaced with a canned clarification",
)
require(
    "reply = retry_user_facing;" in core
    and "if (retry_user_facing && *retry_user_facing)" in core,
    "a nonempty sanitized corrective response is delivered as produced by R2",
)
require(
    "retaining the original model result" in core
    and "couldn't form a reliable response" not in core,
    "if corrective inference returns no text, the code does not substitute a canned user-facing answer",
)
require(
    "If the user says YOUR prior message, answer, or reply was garbled" in core,
    "the main conversation prompt distinguishes feedback about R2's prior output from an unintelligible current user message",
)
require(
    "Do not reinterpret typed text as an audio stream, random sounds, or machine data" in core,
    "primary conversation prompt discourages typed-text misclassification",
)
require(
    "address each important part instead of fixating on one phrase" in core,
    "primary prompt instructs R2 to handle multi-part messages",
)
require(
    "Resolve speaker identity from the actual roles in the current exchange" in core and
    "Learn relationships from explicit statements" in core,
    "main prompt teaches general speaker-role interpretation and relationship learning",
)
require(
    "SPEAKER AND RELATIONSHIP LEARNING:" in core and
    "Retrieved dialogue is evidence about prior events, not the source of current turn roles." in core,
    "active conversation prompt applies relationship learning without mother-specific phrase rules",
)
require(
    "R2_TURN_MEMORY_LIMIT 10" in core and "R2_RECALL_HISTORY_SEARCHES 16" in core,
    "ordinary dialogue has a substantial retrieval budget while explicit recall expands it further",
)
require(
    "r2_query_explicitly_requests_recall(query)" in core,
    "retrieval breadth is selected from the user's actual request",
)

history_filter_start = core.find("static int r2_keyword_is_low_signal(")
history_filter_end = core.find("\nstatic int r2_query_explicitly_requests_recall(", history_filter_start)
require(
    history_filter_start >= 0 and history_filter_end > history_filter_start,
    "memory and Life Log retrieval share a low-signal keyword filter",
)
history_filter = core[history_filter_start:history_filter_end]
for token in ('"hello"', '"hi"', '"hey"', '"r2"', '"speaking"', '"know"'):
    require(
        token in history_filter,
        f"history retrieval skips low-signal greeting/address token {token}",
    )
require(
    "r2_keyword_is_low_signal(keywords[k])" in core,
    "history searches filter greeting/address tokens before spending limited search slots",
)
require(
    "Insert after any leading system context, before dialogue begins." in core and
    "memmove(&copy[context_index + 1], &copy[context_index]" in core,
    "retrieved context is not injected between prior dialogue and the current user turn",
)
require(
    'getenv("R2_OLLAMA_TRACE_PROMPTS")' in core,
    "prompt tracing is disabled unless explicitly opted into",
)
require(
    'strcmp(enabled, "1") != 0' in core,
    "prompt tracing requires an exact opt-in value",
)
require(
    "O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600" in core,
    "prompt trace uses a private, non-overwriting, no-symlink file",
)
require(
    "ollama_trace_prompt_if_enabled(payload);" in core,
    "trace captures the serialized request actually sent to Ollama",
)
require(
    "contains full prompt, memories, and current user text; review before sharing" in core,
    "trace warns that it contains private conversation context",
)

print("Nonresponsive text retry smoke passed.")
, core[definition:end], re.MULTILINE)
    if not literals:
        raise SystemExit(f"Could not decode string literals for {name}")
    try:
        return len("".join(ast.literal_eval(literal) for literal in literals))
    except (SyntaxError, ValueError) as exc:
        raise SystemExit(f"Could not decode {name}: {exc}") from exc

main_prompt_chars = c_string_array_length(
    "r2_conversation_system_prompt", "\n\nstatic char *r2_build_retry_system_prompt(void)"
)
retry_prompt_chars = c_string_array_length(
    "retry_system_prompt", "return xstrdup(retry_system_prompt);"
)
require(main_prompt_chars <= 2193,
        f"foreground policy fits the baseline 4096-context override budget ({main_prompt_chars} chars)")
require(retry_prompt_chars <= 2193,
        f"corrective policy and correction fit the baseline 4096-context override budget ({retry_prompt_chars} chars)")

require(
    "if (retry_after_sanitize_failure ||" in core,
    "empty sanitized replies also receive one corrective retry",
)
require(
    'r2_reply_misclassifies_plain_text(query, reply ? reply : "")' in core,
    "recognized text misclassification triggers the retry",
)
require(
    "static char *r2_validate_corrective_reply(" not in core,
    "corrective model output is not gated by a response classifier or replaced with a canned clarification",
)
require(
    "reply = retry_user_facing;" in core
    and "if (retry_user_facing && *retry_user_facing)" in core,
    "a nonempty sanitized corrective response is delivered as produced by R2",
)
require(
    "retaining the original model result" in core
    and "couldn't form a reliable response" not in core,
    "if corrective inference returns no text, the code does not substitute a canned user-facing answer",
)
require(
    "If the user says YOUR prior message, answer, or reply was garbled" in core,
    "the main conversation prompt distinguishes feedback about R2's prior output from an unintelligible current user message",
)
require(
    "Do not reinterpret typed text as an audio stream, random sounds, or machine data" in core,
    "primary conversation prompt discourages typed-text misclassification",
)
require(
    "address each important part instead of fixating on one phrase" in core,
    "primary prompt instructs R2 to handle multi-part messages",
)
require(
    "Resolve speaker identity from the actual roles in the current exchange" in core and
    "Learn relationships from explicit statements" in core,
    "main prompt teaches general speaker-role interpretation and relationship learning",
)
require(
    "SPEAKER AND RELATIONSHIP LEARNING:" in core and
    "Retrieved dialogue is evidence about prior events, not the source of current turn roles." in core,
    "active conversation prompt applies relationship learning without mother-specific phrase rules",
)
require(
    "R2_TURN_MEMORY_LIMIT 10" in core and "R2_RECALL_HISTORY_SEARCHES 16" in core,
    "ordinary dialogue has a substantial retrieval budget while explicit recall expands it further",
)
require(
    "r2_query_explicitly_requests_recall(query)" in core,
    "retrieval breadth is selected from the user's actual request",
)

history_filter_start = core.find("static int r2_keyword_is_low_signal(")
history_filter_end = core.find("\nstatic int r2_query_explicitly_requests_recall(", history_filter_start)
require(
    history_filter_start >= 0 and history_filter_end > history_filter_start,
    "memory and Life Log retrieval share a low-signal keyword filter",
)
history_filter = core[history_filter_start:history_filter_end]
for token in ('"hello"', '"hi"', '"hey"', '"r2"', '"speaking"', '"know"'):
    require(
        token in history_filter,
        f"history retrieval skips low-signal greeting/address token {token}",
    )
require(
    "r2_keyword_is_low_signal(keywords[k])" in core,
    "history searches filter greeting/address tokens before spending limited search slots",
)
require(
    "Insert after any leading system context, before dialogue begins." in core and
    "memmove(&copy[context_index + 1], &copy[context_index]" in core,
    "retrieved context is not injected between prior dialogue and the current user turn",
)
require(
    'getenv("R2_OLLAMA_TRACE_PROMPTS")' in core,
    "prompt tracing is disabled unless explicitly opted into",
)
require(
    'strcmp(enabled, "1") != 0' in core,
    "prompt tracing requires an exact opt-in value",
)
require(
    "O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600" in core,
    "prompt trace uses a private, non-overwriting, no-symlink file",
)
require(
    "ollama_trace_prompt_if_enabled(payload);" in core,
    "trace captures the serialized request actually sent to Ollama",
)
require(
    "contains full prompt, memories, and current user text; review before sharing" in core,
    "trace warns that it contains private conversation context",
)

print("Nonresponsive text retry smoke passed.")
