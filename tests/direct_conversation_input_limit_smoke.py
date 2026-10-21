#!/usr/bin/env python3
"""Guard shared bounded direct-conversation input and visible rejection."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reader = (ROOT / "r2_input.h").read_text(encoding="utf-8")
start = core.find("int r2_conversation(void)")
end = core.find("\n}\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate r2_conversation().")
body = core[start:end + 3]

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)

read = body.find("r2_read_bounded_line(stdin, line, OLLAMA_MAX_MESSAGE_CHARS,")
check = body.find("if (input_status == R2_INPUT_LINE_TOO_LONG)", read)
quit_check = body.find('!strcasecmp(line, "quit")', check)
talk = body.find("r2_talk(line)", check)
require("malloc((size_t)OLLAMA_MAX_MESSAGE_CHARS + 2U)" in body and
        read >= 0 and check > read,
        "direct conversation allocates a fixed-size buffer and checks reader status")
require("getline(" not in body and
        quit_check > check and talk > check,
        "oversized lines are fully consumed and rejected before commands or model calls")
guard = body[check:quit_check]
require("printf(" in guard and "Nothing from it was sent" in guard and
        "fflush(stdout);" in guard,
        "direct conversation visibly rejects oversized input without truncation")
require("R2_INPUT_LINE_EMBEDDED_NUL" in body and
        "R2_INPUT_LINE_EMBEDDED_NUL" in reader,
        "embedded NUL input is rejected safely")
print("Direct conversation input boundary regression passed.")
