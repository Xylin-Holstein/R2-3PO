#!/usr/bin/env python3
"""Guard one bounded retry for clear typed messages misread as unintelligible input."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = core.find("static int r2_reply_misclassifies_plain_text(")
end = core.find("\nstatic char *r2_retry_direct_answer(", start)
require(start >= 0 and end > start, "plain-text misclassification guard exists")
guard = core[start:end]
for phrase in (
    '"jumbled set of sounds"',
    '"appears to be gibberish"',
    '"unable to understand your message"',
    '"that\'s a bit garbled"',
    '"that’s a bit garbled"',
    "if (words < 5)",
    "Do not override a legitimate conversation about audio, noise, or gibberish.",
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
    ("messages_copy[index].content, query", "retry locates the actual user turn"),
    ("r2_build_retry_system_prompt()", "retry obtains the complete combined system prompt"),
    ("messages_copy, message_count, retry_system_prompt", "retry uses the original history and combined system prompt"),
    ("ollama_authoritative_user_query = query;", "retry preserves active-turn identity"),
    ("conversation_reply_token_budget(query)", "retry respects the normal response budget"),
    ("ollama_chat_with_limit(\n        messages_copy, message_count, retry_system_prompt", "retry does not inject a system message into the middle of dialogue"),
):
    require(phrase in retry, message)

builder_start = core.find("static char *r2_build_retry_system_prompt(void)")
builder_end = core.find("\nstatic char *r2_retry_direct_answer(", builder_start)
require(builder_start >= 0 and builder_end > builder_start,
        "combined corrective system-prompt builder exists")
builder = core[builder_start:builder_end]
for phrase, message in (
    ("RESPONSE CORRECTION:", "builder includes a direct-answer correction instruction"),
    ("Preserve speaker identity and who said what", "builder protects speaker identity and stated meaning"),
    ("attributes words to them that they did not say", "builder forbids fabricated paraphrases"),
    ("r2_conversation_system_prompt, retry_instruction", "builder retains the full R2 prompt before appending correction"),
    ('"%s\\n\\n%s"', "builder separates core and corrective instructions cleanly"),
):
    require(phrase in builder, message)

require(
    "if (retry_after_sanitize_failure ||" in core,
    "empty sanitized replies also receive one corrective retry",
)
require(
    'r2_reply_misclassifies_plain_text(query, reply ? reply : "")' in core,
    "recognized text misclassification triggers the retry",
)
fallback_start = core.find("static char *r2_validate_corrective_reply(")
fallback_end = core.find("static char *r2_retry_direct_answer(", fallback_start)
require(
    fallback_start >= 0 and fallback_end > fallback_start,
    "corrective retry result validation exists",
)
fallback = core[fallback_start:fallback_end]
require(
    "r2_reply_misclassifies_plain_text(query, retry_reply)" in fallback,
    "a repeated misclassification is rejected after the retry",
)
require(
    "couldn't form a reliable response" in fallback and
    "which part you'd like me to respond to" in fallback,
    "failed correction uses an honest clarification rather than repeating fabricated intent",
)
require(
    "r2_validate_corrective_reply(query, retry_user_facing)" in core,
    "sanitized retry result is validated before delivery",
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
    "acknowledge that social meaning" in core,
    "primary prompt preserves emotional and social intent",
)
require(
    "do not answer as though the user were you" in core,
    "primary prompt forbids swapping the user's identity with R2",
)
require(
    "R2_TURN_MEMORY_LIMIT 6" in core and "R2_RECALL_HISTORY_SEARCHES 4" in core,
    "ordinary dialogue uses focused retrieval while explicit recall expands it",
)
require(
    "r2_query_explicitly_requests_recall(query)" in core,
    "retrieval breadth is selected from the user's actual request",
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
