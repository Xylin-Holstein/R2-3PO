#!/usr/bin/env python3
"""Guard bounded terminal input and safe rejection of oversized shell lines."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = shell.find("int r2_shell_run(void)")
end = shell.find("/* ============================================================\n   SHELL STATE ACCESS", start)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate r2_shell_run()")
loop = shell[start:end]

require("#define SHELL_INPUT_MAX       4096" in shell
        and "char line[SHELL_INPUT_MAX + 1];" in loop,
        "terminal input uses the existing fixed input-size limit")
require("fgets(line, sizeof(line), stdin)" in loop
        and "getline(" not in loop,
        "interactive input cannot trigger unbounded getline allocation")
require("if (length >= SHELL_INPUT_MAX)" in loop
        and "Input line exceeds %d bytes; command discarded." in loop,
        "oversized complete lines are rejected before command dispatch")
require("while (!shell_shutdown &&" in loop
        and "fgetc(stdin)" in loop
        and "command discarded." in loop,
        "the remainder of an oversized physical line is drained rather than reinterpreted as a new command")
require("if (shell_shutdown || r2_is_shutting_down())" in loop,
        "interrupted input cannot dispatch a partial command during shutdown")

print("Shell input-bound smoke test passed.")
