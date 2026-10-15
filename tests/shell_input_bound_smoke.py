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

require("#define SHELL_INPUT_MAX       12000" in shell
        and "char line[SHELL_INPUT_MAX + 2];" in loop,
        "terminal input uses a fixed 12,000-byte input buffer")
require("r2_read_bounded_line(stdin, line, SHELL_INPUT_MAX, &input_bytes)" in loop
        and "getline(" not in loop,
        "interactive input uses the shared bounded reader, not unbounded getline allocation")
require("if (input_status == R2_INPUT_LINE_TOO_LONG)" in loop
        and "Nothing from this line was sent to R2." in loop,
        "oversized lines are rejected before command dispatch")
require("R2_INPUT_LINE_EMBEDDED_NUL" in loop
        and "fflush(stdout);" in loop,
        "binary NULs are rejected and the user-facing warning is flushed to stdout")
require("if (shell_shutdown || r2_is_shutting_down())" in loop,
        "interrupted input cannot dispatch a partial command during shutdown")

print("Shell input-bound smoke test passed.")
