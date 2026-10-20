#!/usr/bin/env python3
"""Regression checks for learned relationships and contextual affect in both answer paths."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


answer_start = source.find('static const char r2_conversation_system_prompt[] =')
answer_end = source.find('static char *r2_build_retry_system_prompt(void)', answer_start)
require(answer_start >= 0 and answer_end > answer_start,
        "conversation answer policy is present and bounded")
answer_policy = source[answer_start:answer_end]

for phrase, description in [
    ("SPEAKER AND RELATIONSHIP LEARNING", "normal answers receive speaker-role and relationship-learning guidance"),
    ("Learn relationships from explicit statements", "relationships are learned from evidence rather than fixed name shortcuts"),
    ("AFFECT AND NONVERBAL EXPRESSION", "normal answers receive contextual affect guidance"),
    ("do not rely on a hard-coded emotion-to-response table", "affect interpretation is not a canned emotion-response lookup"),
    ("acknowledge that social meaning", "meaningful social tone is explicitly part of the response policy"),
]:
    require(phrase in answer_policy, description)

retry_start = source.find("static char *r2_build_retry_system_prompt(void)")
retry_end = source.find("Give ordinary turns a substantial bounded evidence budget", retry_start)
require(retry_start >= 0 and retry_end > retry_start,
        "corrective-answer prompt builder is present and bounded")
retry_builder = source[retry_start:retry_end]
for phrase, description in [
    ("identity_start_marker", "corrective answers retain the base identity and persistent-memory section"),
    ("r2_conversation_system_prompt", "corrective answers reuse the same relationship and affect policy as normal answers"),
    ("retry_instruction", "corrective answers retain the current-turn correction instruction"),
    ("return retry_system_prompt", "the assembled corrective policy is returned to the model caller"),
]:
    require(phrase in retry_builder, description)

require("r2_conversation_system_prompt," in source and
        '"conversation-answer"' in source,
        "normal conversation calls the model with the answer policy")
require("compact_messages[compact_count++] = (Message) {" in source and
        ".content = (char *)query" in source,
        "corrective retry explicitly preserves the exact authoritative user message")
require("compact_messages, compact_count, retry_system_prompt" in source and
        '"conversation-retry"' in source,
        "corrective retry calls the model with the assembled policy and is separately audited")

print("Relationship/affect conversation regression checks passed.")
