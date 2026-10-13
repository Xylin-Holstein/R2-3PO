#!/usr/bin/env python3
"""Keep background diagnostics out of R2's interactive conversation terminal."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require('"$R2_ROOT/R2_Log"' in launcher,
        "launcher creates the canonical R2_Log directory as the runtime user")
require('sh -c \'exec "$1" 2>>"$2"\' _ "$R2_EXEC" "$R2_ROOT/R2_Log/Debug.txt"' in launcher,
        "R2 stderr is appended to Debug.txt by the r2 runtime user")
require('R2_FX_DIR="$R2_FX_DIR" \\\n    sh -c' in launcher,
        "diagnostic redirection wraps only the executable and preserves its runtime environment")
require('2>>"$2"' in launcher and '1>>"$2"' not in launcher,
        "only diagnostic stderr is redirected; the user-facing reply stream remains on stdout")
require('[R2 Input Audit] shell_message_chars=%zu' in shell
        and '[R2 Input Audit] r2_talk_received_chars=%zu' in core,
        "input lengths are audited at shell/core boundaries without logging message contents")

hands_marker = core.find('"[HAND %s] %.*s"')
hands_prefix = core[max(0, hands_marker - 120):hands_marker]
require(hands_marker >= 0 and "fprintf" in hands_prefix and "stderr" in hands_prefix,
        "background HANDS output goes to diagnostics rather than the reply stream")

diary_start = core.find("static void *autonomous_thinking(")
diary_end = core.find("return NULL;", diary_start)
diary = core[diary_start:diary_end] if diary_start >= 0 and diary_end > diary_start else ""
require(diary_start >= 0 and "r2_diary_write(" in diary and "reflection" in diary,
        "autonomous reflection is persisted to the private diary")
require("printf(" not in diary and "fprintf(stderr" in diary,
        "private diary text is never printed to the interactive terminal; failures go to diagnostics")

print("R2 console diagnostic isolation smoke test passed.")
