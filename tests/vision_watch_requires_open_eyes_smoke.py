#!/usr/bin/env python3
"""Visual observation must require an open Eyes source before it reports startup."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

start = core.index("int r2_vision_watch_start(void)")
end = core.index("\nint r2_vision_watch_stop(void)", start)
body = core[start:end]

# Refuse startup both before and after taking the worker lifecycle lock.
assert body.count("r2_eyes_status() <= 0") == 2
first_check = body.index("r2_eyes_status() <= 0")
lock = body.index("pthread_mutex_lock(&vision_watch_lock)")
assert first_check < lock
assert body.rfind("r2_eyes_status() <= 0") > lock

handler = shell.index('if (!strcasecmp(argument, "watch start"))')
handler_end = shell.index("\n    }", handler) + 6
watch_start = shell[handler:handler_end]
assert "r2_vision_watch_start() == 0" in watch_start
assert "Could not start visual observation" in watch_start

print("Visual observation refuses startup unless Eyes reports an open source.")
