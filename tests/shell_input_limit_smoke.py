#!/usr/bin/env python3
"""Guard fixed-size shell input and reject overlong lines without truncation."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")

require("#define SHELL_INPUT_MAX       12000" in shell and
        "malloc((size_t)SHELL_INPUT_MAX + 2U)" in shell,
        "shell input uses a bounded buffer at the 12,000-byte ceiling")
loop = shell.find("int r2_shell_run(void)")
read = shell.find("fgets(line, (int)capacity, stdin)", loop)
strip = shell.find("line[--input_bytes] = '\\0';", read)
check = shell.find("if (input_bytes > SHELL_INPUT_MAX)", strip)
dispatch = shell.find("shell_dispatch(input);", check)
require(min(loop, read, strip, check, dispatch) >= 0,
        "bounded input is read and checked in the interactive loop")
require(strip < check < dispatch,
        "oversized input is rejected before shell dispatch or model inference")
guard = shell[check:dispatch]
require("Nothing from this line was sent to R2." in guard,
        "oversized messages are explicitly rejected rather than silently truncated")
require("getline(" not in shell[loop:] and
        "while ((ch = getchar()) != '\\n' && ch != EOF) {}" in guard and
        "continue;" in guard,
        "unbounded getline is avoided and the remainder of an oversized line is drained")
print("Shell input boundary regression passed.")
