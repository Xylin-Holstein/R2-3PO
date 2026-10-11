#!/usr/bin/env python3
"""Run R2 in isolated service mode and verify SIGTERM causes a clean exit."""
from __future__ import annotations

import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCES = [
    "r2.c", "shell.c", "r2_diary.c", "Log.c", "Reality.c",
    "Addiction.c", "Reward.c", "AlternateSelf.c", "Visual.c",
    "Ears.c", "Eyes.c", "R2Sounds.c", "R2Reply.c",
]
LIBS = [
    "-lcurl", "-lsqlite3", "-lpthread", "-ljson-c",
    "-lpulse-simple", "-lpulse", "-lm",
]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="r2-service-signal-") as temporary:
        base = Path(temporary)
        workspace = base / "workspace"
        workspace.mkdir(parents=True, exist_ok=True)
        home = workspace / "R2"
        diary = workspace / "R2_Diary"
        binary = base / "r2-service-smoke"
        log_path = base / "r2-service.log"

        macros = [
            f'-DR2_ROOT="{workspace}"',
            f'-DR2_HOME="{home}"',
            f'-DR2_WORKSPACE="{workspace}"',
            f'-DR2_DIARY_DATABASE="{home / "r2_memory.db"}"',
            f'-DR2_DIARY_DIR="{diary}"',
        ]
        compile_command = [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
            "-I", str(REPO), *macros,
            *(str(REPO / source) for source in SOURCES),
            "-o", str(binary), *LIBS,
        ]
        subprocess.run(compile_command, cwd=REPO, check=True, timeout=120)

        env = os.environ.copy()
        env["R2_SERVICE_MODE"] = "1"
        env["R2_FX_DIR"] = str(base / "FX")
        env["R2_HOME"] = str(home)
        env["R2_TV_SOCKET"] = str(home / "tv-control.sock")

        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(binary)], cwd=workspace, env=env,
                stdout=log, stderr=subprocess.STDOUT,
            )
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    if "R2-3PO ONLINE" in log_path.read_text(encoding="utf-8", errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(0.1)

            startup_log = log_path.read_text(encoding="utf-8", errors="replace")
            if "R2-3PO ONLINE" not in startup_log:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                raise AssertionError("R2 did not reach ONLINE in isolated service mode.\n" + startup_log[-12000:])

            process.send_signal(signal.SIGTERM)
            try:
                return_code = process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError("R2 did not shut down after SIGTERM; possible lost signal or shutdown deadlock.")

        final_log = log_path.read_text(encoding="utf-8", errors="replace")
        if return_code != 0:
            raise AssertionError(f"R2 exited with status {return_code}.\n{final_log[-12000:]}")
        if "[R2-3PO shut down.]" not in final_log:
            raise AssertionError("R2 exited without its clean-shutdown marker.\n" + final_log[-12000:])
        print("PASS: isolated R2 reached ONLINE and shut down cleanly on SIGTERM.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
