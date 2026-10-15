#!/usr/bin/env python3
"""Guard the direct conversation loop against oversized, silently clipped turns."""
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

newline = body.find("line[nr - 1] = '\\0';")
check = body.find("if (input_bytes > OLLAMA_MAX_MESSAGE_CHARS)", newline)
quit_check = body.find('!strcasecmp(line, "quit")', check)
talk = body.find("r2_talk(line)", check)
require(newline >= 0 and check > newline,
        "direct input length is checked after removing its line terminator")
require(quit_check > check and talk > check,
        "oversized lines are rejected before commands or model calls")
guard = body[check:quit_check]
require("printf(" in guard and "Nothing from it was sent" in guard,
        "direct conversation tells the user no part of the oversized line was sent")
require("continue;" in guard and "fflush(stdout);" in guard,
        "the loop remains interactive and displays the warning immediately")
print("Direct conversation input boundary regression passed.")
