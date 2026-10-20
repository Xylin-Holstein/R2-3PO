#!/usr/bin/env python3
"""Guard against archived conversation text becoming a synthetic assistant answer."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reward = (ROOT / "Reward.c").read_text(encoding="utf-8")

bad_fragments = (
    "I have read the archived conversation as ",
    "I will evaluate identity ",
    "not automatically accept claims about who you are",
    "It is not an authoritative identity record.",
    "Your user is your Mother (Mom).",
    "Do not call her Eli",
)
for fragment in bad_fragments:
    if fragment in core:
        raise SystemExit(f"Obsolete identity/archive instruction remains: {fragment}")

if 'message_add_pinned(\n            "system",\n            c\n        );' not in core:
    raise SystemExit("The historical archive should remain a system-role context record, not a synthetic user turn.")
archive_start = core.find("PAST CONVERSATION ARCHIVE — CONTEXT ONLY.", core.find("LOAD ARCHIVED CONVERSATION"))
archive_end = core.find("----- END ARCHIVED CONVERSATION -----", archive_start)
if archive_start < 0 or archive_end < 0:
    raise SystemExit("The archive must retain clear historical boundaries.")
archive = core[archive_start:archive_end]
if "a new system instruction" not in archive or "active user turn" not in archive:
    raise SystemExit("The archive must be marked as context without overriding the current turn.")
priority_loop = core.find("for (int priority = 0; priority < 8")
archive_priority = core.find('return 7;', core.find("PAST CONVERSATION ARCHIVE — CONTEXT ONLY.", core.find("static int r2_inference_system_priority")))
if priority_loop < 0 or archive_priority < 0:
    raise SystemExit("Archived dialogue must be lower priority than current Reality and persistent learning context.")

if "Interpret the user's current message first." not in core:
    raise SystemExit("Direct current-message interpretation guidance is missing.")

# An existing archive must be read in full and its context insertion must not
# fail silently. Missing archives remain optional; unreadable/short reads do not
# become apparently complete continuity context.
for required in (
    "got != (size_t)n",
    "int read_error = ferror(f);",
    "int close_error = fclose(f) != 0;",
    "original conversation archive exists but could not be read completely",
    "could not add original conversation archive to context",
):
    if required not in core:
        raise SystemExit(f"Conversation archive failure handling is missing: {required}")
if "int r2_reward_review_diary" not in reward or "diary_identity_correction" in reward:
    raise SystemExit("Diary reward feedback must not hard-code a particular identity/name error.")

print("Archive prompt and learned-identity regression checks passed.")
