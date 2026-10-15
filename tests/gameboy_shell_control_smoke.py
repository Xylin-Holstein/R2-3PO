#!/usr/bin/env python3
"""Regression checks for safe Game Boy CLI routing and TV control lifecycle."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
gameboy = (ROOT / "GameBoyAdvance.py").read_text(encoding="utf-8")

start = shell.find("static int shell_gameboy(const char *arg)\n{")
end = shell.find("\nstatic int shell_parse_amount(", start)
assert start >= 0 and end > start, "single lifecycle-aware Game Boy handler must exist"
handler = shell[start:end]
assert 'getenv("R2_GAMEBOY_EXECUTABLE")' in handler
assert '"/home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance"' in handler
assert "execv(device, argv)" in handler
assert "system(" not in handler and "popen(" not in handler, "arguments must never enter a shell"
assert "shell_gameboy_sync_verified_events()" in handler
assert "shell_gameboy_log_session_start(&after)" in handler
assert "shell_gameboy_log_session_end(&before" in handler
assert shell.count("static int shell_gameboy(") == 1
assert 'shell_gameboy(shell_starts_with(command, "gameboy ") ? shell_trim(command + 8) : NULL);' in shell
capture_start = shell.find("static int shell_gameboy_capture_events(")
ack_start = shell.find("static int shell_gameboy_ack_event(")
ack_end = shell.find("static void shell_gameboy_sync_verified_events(", ack_start)
assert capture_start >= 0 and ack_start > capture_start and ack_end > ack_start
capture = shell[capture_start:ack_start]
ack = shell[ack_start:ack_end]
assert 'getenv("R2_GAMEBOY_EXECUTABLE")' in capture
assert 'getenv("R2_GAMEBOY_EXECUTABLE")' in ack, (
    "event acknowledgements must use the same configured executable as event reads"
)
assert 'configured && *configured' in ack

for command in (
    '"status"', '"verify"', '"list"', '"power"', '"insert"',
    '"eject"', '"press"', '"move"', '"events"',
):
    assert command in gameboy, f"Game Boy CLI is missing {command}"
assert 'choices=("pockets", "shelf", "box", "room")' in gameboy

assert "static void *tv_control_server(void *unused)" in shell
assert "static int tv_control_start(void)" in shell
assert "static void tv_control_stop_server(void)" in shell
assert "r2_reality_tv_power(1)" in shell
assert "r2_reality_tv_vcr_transport" in shell
assert 'shell_tv(shell_starts_with(command, "tv ") ? shell_trim(command + 3) : NULL);' in shell
assert "tv_control_start() != 0" in shell and "tv_control_stop_server();" in shell

print("R2 TV/VCR and Game Boy shell control-plane smoke passed.")
