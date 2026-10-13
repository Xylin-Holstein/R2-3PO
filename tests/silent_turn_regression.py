#!/usr/bin/env python3
"""Regression checks for silent conversation exits on the legacy main branch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")
    print(f"PASS: {message}")


talk_start = core.index("char *r2_talk(const char *message)")
talk_end = core.find("\nint r2_conversation(", talk_start)
if talk_end < 0:
    talk_end = len(core)
talk = core[talk_start:talk_end]

shell_start = shell.index("static void shell_talk(const char *message)")
shell_end = shell.find("\n/*", shell_start)
shell_talk = shell[shell_start:shell_end if shell_end >= 0 else len(shell)]

require(
    "Conversation generation failed; preserving the user turn and returning a visible response."
    in talk
    and "Your message is still in the conversation; please try again in a moment."
    in talk,
    "failed initial inference returns a visible response instead of returning NULL",
)
require(
    "Action follow-up generation failed; preserving the existing turn and returning a visible response."
    in talk
    and "couldn't generate its follow-up explanation"
    in talk,
    "failed post-action inference returns a visible response",
)
require(
    "if (!reply || !*reply)" in shell_talk
    and "the shell is printing a user-facing failure message" in shell_talk
    and "Your message reached me, but I couldn't answer it this time." in shell_talk,
    "the shell displays a fallback for NULL and empty replies",
)
require(
    "Conversation request failed." not in shell_talk,
    "the shell no longer reports failed conversation requests only on stderr",
)
