#!/usr/bin/env python3
"""Regression checks for Watch's real worker lifecycle and shared conversation path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")

worker_start = shell.index("static void *shell_watch_worker(void *unused)")
worker_end = shell.index("static void shell_watch_start(void)", worker_start)
worker = shell[worker_start:worker_end]
start = shell.index("static void shell_watch_start(void)")
stop = shell.index("static void shell_watch_stop(void)", start)
start_body = shell[start:stop]
stop_end = shell.index("static void shell_watch(const char *argument)", stop)
stop_body = shell[stop:stop_end]

assert "pthread_create(&shell_watch_thread, NULL, shell_watch_worker, NULL)" in start_body
assert "pthread_join(shell_watch_thread, NULL)" in stop_body
assert "atomic_store(&shell_watch_stop_requested, 1)" in stop_body
assert "r2_ears_listen_cancelable(5, shell_watch_cancelled, NULL)" in worker
assert "shell_talk_watch(transcript)" in worker
assert "r2_vision_watch_start()" in worker and "r2_eyes_status() > 0" in worker
assert "r2_vision_watch_stop()" in worker
assert "Coordinator thread : %s" in shell and "Visual observation : %s" in shell
assert "Watch is intended to coordinate" not in shell
assert "R2_WHISPER_MODEL" in start_body and "R2_WHISPER_CLI" in start_body
assert "r2_ears_start()" in start_body and "r2_ears_stop()" in start_body
assert "pthread_mutex_lock(&shell_conversation_lock)" in shell[shell.index("static void shell_talk_watch"):shell.index("static void *shell_watch_worker")]
assert "r2_talk(message)" in shell[shell.index("static void shell_talk_inner"):shell.index("static void shell_talk(const char *message)")]
assert "r2_ears_listen_cancelable(unsigned seconds," in core
assert "if (!transcript && !cancelled)" in core
assert "int r2_watch_start(void);" in header and "int r2_watch_stop(void);" in header
assert "shell_watch_stop();" in shell[shell.index("static void shell_shutdown_r2"):shell.index("static void shell_restart")]
assert "shell_watch_stop();" in shell[shell.index("static void shell_restart"):shell.index("static void shell_clear")]
print("Watch worker startup, cancellation, normal chat handoff, optional visual lifecycle, and shutdown joins passed.")
