#!/usr/bin/env python3
"""Visual observation must not report success without an open Eyes source."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

start = core.index("int r2_vision_watch_start(void)")
end = core.index("\nint r2_vision_watch_stop(void)", start)
body = core[start:end]

# Check both the fast path and the locked recheck so an already-running or
# newly-starting worker cannot be reported as started with Eyes unavailable.
assert body.count("r2_eyes_status() <= 0") == 2
assert body.index("r2_eyes_status() <= 0") < body.index("pthread_mutex_lock(&vision_watch_lock)")
assert body.rfind("r2_eyes_status() <= 0") > body.index("pthread_mutex_lock(&vision_watch_lock)")

handler = shell.index('if (!strcasecmp(argument, "watch start"))')
handler_end = shell.index("\n    }", handler) + 6
watch_start = shell[handler:handler_end]
assert "r2_vision_watch_start() == 0" in watch_start
assert "Could not start visual observation" in watch_start

print("Visual observation refuses to start when Eyes has no open source.")
