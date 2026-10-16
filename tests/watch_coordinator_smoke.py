#!/usr/bin/env python3
"""Regression checks for the real Watch worker lifecycle and normal chat handoff."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

def section(source: str, start_marker: str, end_marker: str) -> str:
    start = source.find(start_marker)
    end = source.find(end_marker, start)
    if start < 0 or end < 0:
        raise SystemExit(f"Could not isolate source section: {start_marker}")
    return source[start:end]

worker = section(core, "static void *watch_worker(void *unused)", "int r2_watch_active(void)")
start = section(core, "int r2_watch_start(void)", "int r2_watch_stop(void)")
stop = section(core, "int r2_watch_stop(void)", "void r2_watch_status(void)")
shutdown_start = core.find("void r2_shutdown(void)")
shutdown_end = core.find("/* ============================================================\n   PROGRAM ENTRY", shutdown_start)
if shutdown_start < 0 or shutdown_end < 0:
    raise SystemExit("Could not isolate r2_shutdown()")
shutdown = core[shutdown_start:shutdown_end]
shell_watch = section(shell, "static void shell_watch_status(void)", "static void shell_watch(const char *argument)")
ears_stop_start = shell.find('if (!strcasecmp(argument, "stop"))', shell.find("static void shell_ears("))
ears_stop_end = shell.find('if (!strcasecmp(argument, "status"))', ears_stop_start)
ears_stop = shell[ears_stop_start:ears_stop_end]

checks = {
    "Watch uses cancellable utterance capture": "r2_ears_listen_utterance_cancelable(" in worker,
    "Watch keeps its owned live microphone open between utterances": "watch_open_audio(&opened_by_watch)" in worker and "watch_close_audio(&opened_by_watch)" in worker,
    "silence-only windows never become chat turns": 'if (!*transcript)' in worker and "continue;" in worker,
    "recognized speech enters the normal public chat backend": "r2_talk(transcript)" in worker,
    "shutdown is checked before starting a new speech-driven chat turn": "if (shutting_down || !atomic_load(&watch_running))" in worker,
    "Watch never opens Eyes or the camera implicitly": "r2_vision_watch_start(" not in worker,
    "Watch start requires a configured local model and executable": "watch_asr_configured()" in start and "pthread_create(" in start,
    "Watch stop clears the active state and joins the worker": "atomic_store(&watch_running, 0)" in stop and "pthread_join(watch_thread, NULL)" in stop,
    "shutdown stops background inference before joining sensory workers": "r2_stop_thinking();" in shutdown and shutdown.find("r2_stop_thinking();") < shutdown.find("r2_vision_watch_stop();") < shutdown.find("r2_watch_stop();"),
    "shell Watch status comes from the core worker": "r2_watch_active()" in shell_watch,
    "shell no longer maintains a duplicate Watch state flag": "watch_running" not in shell,
    "ears stop joins Watch before closing the shared source": "r2_watch_stop();" in ears_stop and ears_stop.find("r2_watch_stop();") < ears_stop.find("r2_ears_stop();"),
    "shell reports the actual local ASR-to-chat behavior": "natural pause" in shell_watch and "normal conversation pipeline" in shell_watch,
}
for label, passed in checks.items():
    if not passed:
        print(f"FAIL: {label}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {label}")
print("Watch coordinator lifecycle regression passed.")
