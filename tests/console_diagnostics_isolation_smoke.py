#!/usr/bin/env python3
"""Keep diagnostics on stderr/Debug.txt and preserve stdout for user-facing replies."""
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

require('"$R2_ROOT/R2_Log"' in launcher, "launcher uses the canonical R2_Log directory")
require('R2_LOG_DIR="$R2_ROOT/R2_Log"' in launcher and 'sh -c \'exec "$@" 2>>"$R2_LOG_DIR/Debug.txt"\' _ "$R2_EXEC"' in launcher,
        "R2 stderr is appended to Debug.txt as runtime user r2")
require('2>>"$R2_LOG_DIR/Debug.txt"' in launcher and '1>>"$R2_LOG_DIR/Debug.txt"' not in launcher,
        "only diagnostic stderr is redirected; conversation stdout stays on the terminal")
require('R2_FX_DIR="$R2_FX_DIR" \\' in launcher,
        "the sound asset directory remains part of the runtime environment")
require('static void *reality_needs_worker(void *unused)' in core and 'r2_log_event(R2_LOG_WORLD' in core,
        "background needs diagnostics use the Life Log rather than printing into replies")
require('fprintf(stderr' in shell, "shell diagnostics use stderr")
require('r2_reply_sanitize(reply)' in core, "user-facing replies pass through output sanitization")
print("R2 console diagnostic isolation smoke test passed.")
