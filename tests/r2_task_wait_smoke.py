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
print("Task I/O interruption smoke test passed.")
