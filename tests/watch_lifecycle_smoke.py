#!/usr/bin/env python3
"""Watch must never claim ACTIVE until a real sensory coordinator exists."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

start = core.index("int r2_watch_active(void)")
end = core.index("void r2_request_shutdown(void)", start)
watch = core[start:end]

assert "return 0;" in watch.split("int r2_watch_start(void)", 1)[0]
start_fn = watch.split("int r2_watch_start(void)", 1)[1].split("int r2_watch_stop(void)", 1)[0]
assert "return -1;" in start_fn
assert "watch_unavailable" in start_fn
assert "watch_running = 1;" not in start_fn
assert "NOT IMPLEMENTED (no sensory coordinator worker)" in watch
assert "r2_watch_start()" in shell
print("Watch lifecycle smoke check passed: unavailable Watch cannot report ACTIVE.")
