#!/usr/bin/env python3
"""Stopping periodic visual observation must abort its in-flight inference before join."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

assert "static _Thread_local int vision_watch_worker_context = 0;" in core
abort_start = core.index("int r2_ollama_vision_request_should_abort(void)")
abort_end = core.index("\n}", abort_start)
abort = core[abort_start:abort_end]
assert "(vision_watch_worker_context && !atomic_load(&vision_watch_running))" in abort

worker_start = core.index("static void *vision_watch_worker(void *unused)")
worker_end = core.index("\nint r2_vision_watch_start(void)", worker_start)
worker = core[worker_start:worker_end]
assert "vision_watch_worker_context = 1;" in worker
assert "vision_watch_worker_context = 0;" in worker

stop_start = core.index("int r2_vision_watch_stop(void)")
stop_end = core.index("\nint r2_vision_watch_active(void)", stop_start)
stop = core[stop_start:stop_end]
assert stop.index("atomic_store(&vision_watch_running, 0)") < stop.index("pthread_join(vision_watch_thread, NULL)")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
assert 'json_object_get_boolean(done)' in visual
assert 'strcmp(json_object_get_string(role), "assistant") == 0' in visual
assert '"vision_invalid_completion_contract"' in visual
print("Visual Watch stop cancels its own in-flight inference before joining, without aborting direct vision requests.")
