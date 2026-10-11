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
    '"i think you\'re trying to say"',
    '"i think you’re trying to say"',
    '"you are trying to say"',
    '"is that what you mean"',
    "if (words < 5)",
    "Do not override a legitimate conversation about audio, noise, or gibberish.",
):
    require(phrase in guard, f"guard checks {phrase}")

start = core.find("static char *r2_retry_direct_answer(")
end = core.find("\nstatic char *chat_with_relevant_memories(", start)
require(start >= 0 and end > start, "corrective retry helper exists")
retry = core[start:end]
for phrase, message in (
    ("RESPONSE CORRECTION:", "retry adds a direct-answer correction instruction"),
    ("Preserve speaker identity and who said what", "retry protects speaker identity and the user's stated meaning"),
    ("attributes words to them that they did not say", "retry forbids fabricated paraphrases"),
    ("messages_copy[index].content, query", "retry locates the actual user turn"),
    ("r2_conversation_system_prompt", "full R2 system prompt is shared with the retry"),
    ("snprintf(retry_system_prompt, prompt_size, \"%s\\n\\n%s\"", "retry appends correction without dropping core instructions"),
    ("messages_copy, message_count, retry_system_prompt", "retry uses the original history and combined system prompt"),
    ("ollama_authoritative_user_query = query;", "retry preserves active-turn identity"),
    ("conversation_reply_token_budget(query)", "retry respects the normal response budget"),
    ("r2_conversation_system_prompt, retry_instruction", "retry combines identity and correction instructions"),
    ("ollama_chat_with_limit(\n        messages_copy, message_count, retry_system_prompt", "retry does not inject a system message into the middle of dialogue"),
):
    require(phrase in retry, message)

require(
    "if (retry_after_sanitize_failure ||" in core,
    "empty sanitized replies also receive one corrective retry",
)
require(
    'r2_reply_misclassifies_plain_text(query, reply ? reply : "")' in core,
    "recognized text misclassification triggers the retry",
)
require(
    "Direct-answer retry produced no safe text." in core and "if (retry_after_sanitize_failure)" in core,
    "failed corrective retry propagates an empty result to the existing recovery path",
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

print("Nonresponsive text retry smoke passed.")
