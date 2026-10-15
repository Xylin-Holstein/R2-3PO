#!/usr/bin/env python3
"""Guard against stderr-only failures in R2's direct interactive conversation loop."""
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
    print(f"PASS: {message}")

require("if (!reply || !*reply)" in body,
        "direct conversation handles NULL and empty replies")
null_start = body.find("if (!reply || !*reply)")
null_end = body.find("\n        }", null_start)
null_path = body[null_start:null_end + 10] if null_start >= 0 and null_end >= 0 else ""
require("printf(" in null_path and "R2-3PO:" in null_path,
        "failed turn produces a visible user-facing stdout fallback")
require("fflush(stdout);" in null_path,
        "fallback is flushed immediately rather than waiting for another turn")
print("Direct conversation failure-path regression passed.")
