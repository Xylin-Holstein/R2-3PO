#!/usr/bin/env python3
"""Regression checks for the R2 shell's Game Boy control-plane routing."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
gameboy = (ROOT / "GameBoyAdvance.py").read_text(encoding="utf-8")

start = shell.find("static void shell_gameboy(const char *argument)")
end = shell.find("/* ============================================================\n   COMMAND DISPATCH", start)
assert start >= 0 and end > start, "Game Boy command handler must precede shell dispatch"
handler = shell[start:end]
assert 'getenv("R2_GAMEBOY_EXECUTABLE")' in handler
assert '"/home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance"' in handler
assert "execv(executable, argv)" in handler
assert "system(" not in handler and "popen(" not in handler, "filenames must never enter a shell"
for command in ('"status"', '"verify"', '"list"', '"power"', '"insert"', '"eject"', '"press"', '"move"', '"events"'):
    assert command in handler, f"missing Game Boy operation {command}"
assert 'shell_gameboy(shell_starts_with(command, "gameboy ") ? shell_trim(command + 8) : NULL);' in shell
assert 'sub.add_parser("status"' in gameboy and 'sub.add_parser("insert"' in gameboy
assert 'choices=("pockets", "shelf", "box", "room")' in gameboy
print("R2 Game Boy shell control-plane smoke passed.")
