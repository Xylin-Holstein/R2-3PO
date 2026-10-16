#!/usr/bin/env python3
"""Regression checks that failed inference never silently drops a conversation turn."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


talk_start = core.index("char *r2_talk(const char *message)")
talk_end = core.find("\nint r2_conversation(", talk_start)
if talk_end < 0:
    talk_end = len(core)
talk = core[talk_start:talk_end]

impl_start = core.index("static char *r2_talk_impl(const char *message)")
impl_end = core.find("\nchar *r2_talk(const char *message)", impl_start)
if impl_end < 0:
    raise SystemExit("Could not isolate the conversation implementation.")
implementation = core[impl_start:impl_end]

shell_start = shell.index("static void shell_talk(const char *message)")
shell_end = shell.find("\n/*", shell_start)
shell_talk = shell[shell_start:shell_end if shell_end >= 0 else len(shell)]

require(
    "char *reply = r2_talk_impl(message);" in talk
    and "if (!reply || !*reply)" in talk
    and "returning an explicit failure message" in talk,
    "public conversation API detects NULL and empty replies",
)
require(
    "The message may not have been saved to conversation history; please try again." in talk
    and "reply = xstrdup(" in talk,
    "failed inference produces a visible allocated fallback instead of returning NULL",
)
require(
    "The action result is still in the conversation; please ask me again" in implementation
    and "does not rerun the completed action" in implementation,
    "failed post-action generation preserves the completed action result without replaying the action",
)
require(
    "if (!reply || !*reply)" in shell_talk
    and "the shell is showing a fallback instead of silently dropping the turn" in shell_talk
    and "Your message reached me, but I couldn't answer it this time." in shell_talk,
    "shell talk displays a user-facing fallback if the API returns no usable text",
)
require(
    "free(reply);" in shell_talk
    and "Conversation request failed." not in shell_talk,
    "the fallback path safely frees an empty reply and avoids stderr-only failure reporting",
)
