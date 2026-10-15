#!/usr/bin/env python3
"""Guard shell input bounds without silently truncating user messages."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")

require("#define SHELL_INPUT_MAX       12000" in shell,
        "shell input ceiling matches the core's bounded current-turn size")
loop = shell.find("int r2_shell_run(void)")
read = shell.find("getline(", loop)
strip = shell.find("line[length - 1] = '\\0';", read)
check = shell.find("if (input_bytes > SHELL_INPUT_MAX)", strip)
dispatch = shell.find("shell_dispatch(input);", check)
require(min(loop, read, strip, check, dispatch) >= 0,
        "input limit is checked in the interactive read loop")
require(strip < check < dispatch,
        "oversized input is rejected before shell dispatch or model inference")
guard = shell[check:dispatch]
require("Nothing from this line was sent to R2." in guard,
        "oversized messages are explicitly rejected rather than silently truncated")
require("strlen(line)" in guard and "continue;" in guard,
        "the full line is measured and rejected before it can enter conversation")
print("Shell input boundary regression passed.")
