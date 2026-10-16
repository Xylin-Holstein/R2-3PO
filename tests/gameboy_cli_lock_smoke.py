#!/usr/bin/env python3
"""Verify concurrent CLI power-on requests cannot launch duplicate emulators."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "GameBoyAdvance.py").read_text(encoding="utf-8")
lock_at = SOURCE.find('with (STATE / ".console.lock").open("a+") as lock_file:')
flock_at = SOURCE.find("fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)", lock_at)
connect_at = SOURCE.find("db = connect()", flock_at)
dispatch_at = SOURCE.find('if command == "status":', connect_at)
assert lock_at >= 0 and flock_at > lock_at and connect_at > flock_at and dispatch_at > connect_at, (
    "the inter-process lock must cover database connection and complete command dispatch"
)

CONSOLE = ROOT / "GameBoyAdvance.py"


def call(env: dict[str, str], *args: str) -> dict:
    proc = subprocess.run(
        [sys.executable, str(CONSOLE), "--json", *args],
        env=env, text=True, capture_output=True, timeout=10,
    )
    if not proc.stdout.strip():
        raise AssertionError(f"No JSON output for {args}: {proc.stderr}")
    return json.loads(proc.stdout)


with tempfile.TemporaryDirectory(prefix="r2-gameboy-lock-") as temp:
    root = Path(temp) / "device"
    rom_dir = root / "Cartridges"
    rom_dir.mkdir(parents=True)
    rom = bytearray(0x200)
    rom[0xA0:0xAC] = b"LOCK TEST   "
    (rom_dir / "LockTest.gba").write_bytes(rom)

    launches = Path(temp) / "emulator-launches.txt"
    emulator = Path(temp) / "fake-mgba"
    emulator.write_text(
        "#!/usr/bin/env python3\n"
        "import time\n"
        "from pathlib import Path\n"
        f"with Path({str(launches)!r}).open('a', encoding='utf-8') as out:\n"
        "    out.write('launch\\n')\n"
        "time.sleep(60)\n",
        encoding="utf-8",
    )
    emulator.chmod(0o755)
    env = os.environ.copy()
    env["R2_GAMEBOY_ROOT"] = str(root)
    env["R2_MGBA_EXECUTABLE"] = str(emulator)
    assert call(env, "insert", "LockTest.gba")["ok"]

    children = [
        subprocess.Popen(
            [sys.executable, str(CONSOLE), "--json", "power", "on"],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        for _ in range(2)
    ]
    outputs = []
    try:
        for child in children:
            stdout, stderr = child.communicate(timeout=10)
            if not stdout.strip():
                raise AssertionError(f"Power-on produced no JSON: {stderr}")
            outputs.append(json.loads(stdout))
        assert sum(bool(result.get("ok")) for result in outputs) == 1, outputs
        launch_count = len(launches.read_text(encoding="utf-8").splitlines())
        assert launch_count == 1, f"Expected one emulator launch, got {launch_count}"
        assert call(env, "power", "off")["ok"]
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=3)
        try:
            state = call(env, "status")
            pid = state.get("emulator_pid")
            if pid:
                import signal
                os.kill(int(pid), signal.SIGKILL)
        except Exception:
            pass

print("Game Boy CLI process-serialization regression passed.")
