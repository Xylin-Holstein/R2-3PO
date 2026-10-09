#!/usr/bin/env python3
"""Isolated state-machine smoke test for the virtual Game Boy console."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CONSOLE = REPO / "GameBoyAdvance.py"


def run(root: Path, emulator: Path, *args: str, expect_ok: bool = True) -> dict:
    env = os.environ.copy()
    env["R2_GAMEBOY_ROOT"] = str(root)
    env["R2_MGBA_EXECUTABLE"] = str(emulator)
    proc = subprocess.run(
        [sys.executable, str(CONSOLE), "--json", *args],
        text=True, capture_output=True, env=env, timeout=10,
    )
    if not proc.stdout.strip():
        raise AssertionError(f"No JSON output for {args}: {proc.stderr}")
    result = json.loads(proc.stdout)
    if expect_ok and proc.returncode != 0:
        raise AssertionError(f"{args} failed: {result} {proc.stderr}")
    if not expect_ok and proc.returncode == 0:
        raise AssertionError(f"{args} unexpectedly succeeded: {result}")
    return result


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="r2-gameboy-test-") as temp:
        root = Path(temp) / "device"
        cartridges = root / "Cartridges"
        cartridges.mkdir(parents=True)
        emulator = Path(temp) / "fake-mgba"
        emulator.write_text(
            "#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n",
            encoding="utf-8",
        )
        emulator.chmod(0o755)

        # Powering on empty turns on the device but must not launch an emulator.
        result = run(root, emulator, "power", "on")
        assert result["power_state"] == "on"
        assert result["cartridge_inserted"] is False
        assert result["emulator_pid"] is None
        run(root, emulator, "power", "off")

        # Make a small valid-looking loose GBA ROM with a title in its header.
        rom = bytearray(0x200)
        rom[0xA0:0xAC] = b"TEST MARIO  "
        (cartridges / "Mario.gba").write_bytes(rom)
        (cartridges / "Second.gba").write_bytes(rom)

        result = run(root, emulator, "insert", "Mario.gba")
        assert result["cartridge_inserted"] is True
        assert result["cartridge_title"].strip() == "TEST MARIO"

        result = run(root, emulator, "power", "on")
        assert result["power_state"] == "on"
        assert result["game_running"] is True
        assert result["emulator_pid"] is not None

        # Cartridge replacement while powered on is rejected and changes nothing.
        result = run(root, emulator, "insert", "Second.gba", expect_ok=False)
        assert "Turn it off" in result["message"]
        assert Path(result["cartridge_path"]).name == "Mario.gba"

        result = run(root, emulator, "eject", expect_ok=False)
        assert result["cartridge_inserted"] is True

        # Power off ends the emulator session without ejecting the cartridge.
        result = run(root, emulator, "power", "off")
        assert result["power_state"] == "off"
        assert result["cartridge_inserted"] is True
        assert result["emulator_pid"] is None
        assert "No in-game save command or save state" in result["message"]

        run(root, emulator, "eject")
        result = run(root, emulator, "status")
        assert result["power_state"] == "off"
        assert result["cartridge_inserted"] is False
        assert result["game_running"] is False

    print("Game Boy console state-machine smoke test passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
