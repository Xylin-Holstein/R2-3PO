#!/usr/bin/env python3
"""Regression checks for safe Game Boy CLI routing and TV control lifecycle."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
gameboy = (ROOT / "GameBoyAdvance.py").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

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

# Adapter event delivery is at-least-once until acknowledgement. Replay must
# reuse its durable Life Log record and apply the learning signal only once.
sync_start = core.index("static int sync_gameboy_verified_events(void)")
sync_end = core.index("\nstatic char *process_tools_with_status", sync_start)
sync = core[sync_start:sync_end]
assert "gameboy_find_life_event_for_console_event(event_id)" in sync
assert 'r2_log_event(\n                R2_LOG_WORLD, "gameboy_game_event"' in sync
assert "r2_reward_apply_once(" in sync
assert 'snprintf(reward_source, sizeof(reward_source), "game_event:%lld"' in sync
assert "if (reward_rc < 0)" in sync and "leaving it pending" in sync
assert 'run_gameboy_console("ack_event", event_id_text' in sync
helper_start = core.index("static int64_t gameboy_find_life_event_for_console_event")
helper_end = core.index("static int sync_gameboy_verified_events", helper_start)
helper = core[helper_start:helper_end]
assert "WHERE event_type='gameboy_game_event' AND details LIKE ?" in helper
assert "sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT)" in helper

assert "static void *tv_control_server(void *unused)" in shell
assert "static int tv_control_start(void)" in shell
assert "static void tv_control_stop_server(void)" in shell
assert "r2_reality_tv_power(1)" in shell
assert "r2_reality_tv_vcr_transport" in shell
assert 'shell_tv(shell_starts_with(command, "tv ") ? shell_trim(command + 3) : NULL);' in shell
assert "tv_control_start() != 0" in shell and "tv_control_stop_server();" in shell

print("R2 TV/VCR and Game Boy shell control-plane smoke passed.")
