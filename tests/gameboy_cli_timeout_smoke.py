#!/usr/bin/env python3
"""Guard Game Boy helper subprocesses against indefinite shell hangs."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "shell.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = source.find("static int shell_wait_child_bounded(")
end = source.find("static int shell_gameboy_capture_events(", start)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate bounded child wait helper")
waiter = source[start:end]
require("waitpid(child, status_out, WNOHANG)" in waiter
        and "now - started >= timeout_ms" in waiter
        and "kill(-child, SIGKILL)" in waiter,
        "child waits use monotonic deadlines and kill the complete process group")

capture_start = source.find("static int shell_gameboy_capture_events(")
capture_end = source.find("static int shell_gameboy_ack_event(", capture_start)
if capture_start < 0 or capture_end <= capture_start:
    raise SystemExit("FAIL: could not isolate Game Boy event capture")
capture = source[capture_start:capture_end]
require("setpgid(0, 0)" in capture and "setpgid(child, child)" in capture
        and "poll(&output_fd, 1," in capture
        and "now - started >= 5000" in capture,
        "event capture polls output and applies a five-second total deadline")
require("shell_wait_child_bounded(child, 1000, &status)" in capture
        and "timed_out" in capture and "read_failed" in capture,
        "event capture bounds the final wait and discards timeout/truncated output")

ack_start = source.find("static int shell_gameboy_ack_event(")
ack_end = source.find("static void shell_gameboy_sync_verified_events(", ack_start)
if ack_start < 0 or ack_end <= ack_start:
    raise SystemExit("FAIL: could not isolate Game Boy event acknowledgement")
ack = source[ack_start:ack_end]
require("shell_wait_child_bounded(child, 5000, &status)" in ack
        and "setpgid(0, 0)" in ack,
        "event acknowledgement cannot block the shell indefinitely")

print("Game Boy CLI timeout smoke test passed.")
