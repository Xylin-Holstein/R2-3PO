#!/usr/bin/env python3
"""Guard task pipe/wait handling against signal-interruption false successes."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
start = core.find("static void execute_task(")
end = core.find("/* ============================================================\n   HANDS WORKER", start)
if start < 0 or end <= start:
    print("FAIL: could not isolate execute_task()", file=sys.stderr)
    raise SystemExit(1)
task = core[start:end]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


require("if (got < 0 && errno == EINTR)" in task,
        "transient signal interruptions retry task-output reads")
require("if (got < 0) {\n            output_read_failed = 1;" in task,
        "real pipe read failures are recorded rather than treated as EOF")
require("do {\n        waited = waitpid(pid, &status, 0);\n    } while (waited < 0 && errno == EINTR);" in task,
        "child waits retry EINTR until the process status is actually collected")
require("!output_read_failed && waited == pid && WIFEXITED(status)" in task,
        "task success requires a valid wait result and successful output collection")
require("while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}" in task,
        "allocation-failure cleanup does not abandon a child on interrupted wait")

start_background = core.find("static char *start_background_task(")
end_background = core.find("/* ============================================================\n   COMPLETED TASKS", start_background)
if start_background < 0 or end_background <= start_background:
    raise SystemExit("FAIL: could not isolate start_background_task()")
background = core[start_background:end_background]
require("if (!t->command || !id)" in background
        and "free(t->command)" in background and "free(id)" in background,
        "background task allocation failure cannot enqueue a NULL command or leak partial state")
require("if (dup2(pipefd[1], STDOUT_FILENO) < 0" in task
        and "_exit(126)" in task,
        "child output redirection failures terminate with a nonzero status")
require("if (!c->command)" in task,
        "task command allocation failure is handled before launching a child")
require('c->command ? c->command : "[command unavailable]"' in core
        and 'c->output ? c->output : "[output unavailable]"' in core,
        "completed-task collection remains safe if diagnostic allocations fail")
require("output_read_failed = 1;\n            (void)kill(-pid, SIGKILL);" in task,
        "output-buffer allocation failure terminates the complete task process group")
require("if (setpgid(0, 0) != 0)" in task
        and "setpgid(pid, pid)" in task,
        "task commands and descendants share a cancellable process group")
require("poll(&output_fd, 1, 250)" in task
        and "if (shutting_down && !shutdown_terminated_task)" in task
        and "(void)kill(-pid, SIGKILL);" in task,
        "silent active tasks are cancelled promptly when shutdown begins")
shutdown_start = core.find("void r2_shutdown(void)")
shutdown = core[shutdown_start:] if shutdown_start >= 0 else ""
require("CompletedTask *completed_tasks = completed_head" in shutdown
        and "completed_head = NULL;" in shutdown
        and "completed_tail = NULL;" in shutdown
        and "free(completed_tasks->command);" in shutdown
        and "free(completed_tasks->output);" in shutdown,
        "shutdown clears completed task results so they cannot leak or cross into a reinitialized session")

console_bridge_start = core.find("static int run_gameboy_console(")
console_bridge_end = core.find("static int gameboy_json_bool(", console_bridge_start)
if console_bridge_start < 0 or console_bridge_end <= console_bridge_start:
    raise SystemExit("FAIL: could not isolate run_gameboy_console()")
console_bridge = core[console_bridge_start:console_bridge_end]
require("while (waitpid(pid, &status, 0) < 0)" in console_bridge
        and "close(pipes[0]);" in console_bridge
        and "if (errno == EINTR) continue;" in console_bridge,
        "console bridge reaps its child after output errors and retries interrupted waits")

shell = (ROOT / "shell.c").read_text(encoding="utf-8")
capture_start = shell.find("static int shell_gameboy_capture_events(")
capture_end = shell.find("static int shell_gameboy_ack_event(", capture_start)
if capture_start < 0 or capture_end <= capture_start:
    raise SystemExit("FAIL: could not isolate shell_gameboy_capture_events()")
capture = shell[capture_start:capture_end]
require("int read_failed = 0, timed_out = 0;" in capture
        and "read_failed = 1;" in capture
        and "return !read_failed && !timed_out && wait_result == 0" in capture
        and "WIFEXITED(status)" in capture,
        "shell does not accept partial or timed-out Game Boy event JSON")

completed_start = core.find("static char *collect_completed(void)")
completed_end = core.find("MARKER EXTRACTION", completed_start)
if completed_start < 0 or completed_end <= completed_start:
    raise SystemExit("FAIL: could not isolate collect_completed()")
collect = core[completed_start:completed_end]
initial_alloc_failure = collect.find("char *out =")
initial_alloc_failure = collect.find("if (!out)", initial_alloc_failure)
cleanup = collect.find("CompletedTask *remaining = head", initial_alloc_failure)
require(initial_alloc_failure >= 0 and cleanup > initial_alloc_failure
        and "free(remaining->command);" in collect[cleanup:]
        and "free(remaining->output);" in collect[cleanup:]
        and "free(remaining);" in collect[cleanup:]
        and "return NULL;" in collect[cleanup:],
        "completed task records are freed if the initial collection buffer allocation fails")

print("Task/subprocess I/O interruption smoke test passed.")
