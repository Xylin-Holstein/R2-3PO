#!/usr/bin/env python3
"""Guard fixed-size direct conversation input and reject oversized lines."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
start = core.find("int r2_conversation(void)")
end = core.find("\n}\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate r2_conversation().")
body = core[start:end + 3]

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)

newline = body.find("line[--input_bytes] = '\\0';")
check = body.find("if (input_bytes > OLLAMA_MAX_MESSAGE_CHARS)", newline)
quit_check = body.find('!strcasecmp(line, "quit")', check)
talk = body.find("r2_talk(line)", check)
require("malloc((size_t)OLLAMA_MAX_MESSAGE_CHARS + 2U)" in body and
        "fgets(line, (int)linecap, stdin)" in body and
        newline >= 0 and check > newline,
        "direct conversation uses a fixed-size buffer and strips newline before checking")
require("getline(" not in body and
        "while ((ch = getchar()) != '\\n' && ch != EOF) {}" in body[check:quit_check] and
        quit_check > check and talk > check,
        "oversized lines are drained and rejected before commands or model calls")
guard = body[check:quit_check]
require("printf(" in guard and "Nothing from it was sent" in guard,
        "direct conversation tells the user no part of the oversized line was sent")
require("continue;" in guard and "fflush(stdout);" in guard,
        "the loop remains interactive and displays the warning immediately")
print("Direct conversation input boundary regression passed.")
