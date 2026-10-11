#!/usr/bin/env python3
"""Run R2 in isolated service mode and verify media logging and SIGTERM shutdown."""
from __future__ import annotations

import os
import signal
import socket
import sqlite3
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
        memory_db = home / "r2_memory.db"
        binary = base / "r2-service-smoke"
        log_path = base / "r2-service.log"

        macros = [
            f'-DR2_ROOT="{workspace}"',
            f'-DR2_HOME="{home}"',
            f'-DR2_WORKSPACE="{workspace}"',
            f'-DR2_DIARY_DATABASE="{memory_db}"',
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
        env["R2_TV_SOCKET"] = str(home / "tv-control.sock")

        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(binary)], cwd=workspace, env=env,
                stdout=log, stderr=subprocess.STDOUT,
            )
            return_code = None
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        break
                    try:
                        if "R2-3PO ONLINE" in log_path.read_text(
                            encoding="utf-8", errors="replace"
                        ):
                            break
                    except OSError:
                        pass
                    time.sleep(0.1)

                startup_log = log_path.read_text(encoding="utf-8", errors="replace")
                if "R2-3PO ONLINE" not in startup_log:
                    raise AssertionError(
                        "R2 did not reach ONLINE in isolated service mode.\n"
                        + startup_log[-12000:]
                    )

                # Exercise the real Unix-socket -> C shell -> Life Log media bridge.
                socket_path = home / "tv-control.sock"
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(3.0)
                    client.connect(str(socket_path))
                    client.sendall(b"media vcr_playback_started|/tmp/audit-film.mp4\n")
                    response_chunks = []
                    while True:
                        chunk = client.recv(4096)
                        if not chunk:
                            break
                        response_chunks.append(chunk)
                response = b"".join(response_chunks).decode("utf-8", errors="replace")
                if not response.startswith("OK media event recorded"):
                    raise AssertionError(f"Media event socket command failed: {response!r}")

                with sqlite3.connect(
                    f"file:{memory_db}?mode=ro", uri=True, timeout=5.0
                ) as db:
                    row = db.execute(
                        "SELECT summary FROM r2_log_events "
                        "WHERE event_type=? AND summary LIKE ? ORDER BY id DESC LIMIT 1",
                        ("vcr_playback_started", "%/tmp/audit-film.mp4%"),
                    ).fetchone()
                if row is None:
                    raise AssertionError(
                        "VCR playback event was not persisted to the Life Log."
                    )
            finally:
                if process.poll() is None:
                    try:
                        process.send_signal(signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    return_code = process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    raise AssertionError(
                        "R2 did not shut down after SIGTERM; possible lost signal or shutdown deadlock."
                    )

        final_log = log_path.read_text(encoding="utf-8", errors="replace")
        if return_code != 0:
            raise AssertionError(f"R2 exited with status {return_code}.\n{final_log[-12000:]}")
        if "[R2-3PO shut down.]" not in final_log:
            raise AssertionError(
                "R2 exited without its clean-shutdown marker.\n" + final_log[-12000:]
            )
        print("PASS: R2 reached ONLINE, logged a media event, and shut down cleanly on SIGTERM.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
