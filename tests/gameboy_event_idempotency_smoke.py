#!/usr/bin/env python3
"""Regression checks for idempotent adapter-verified Game Boy event ingestion."""
from pathlib import Path

source = Path("r2.c").read_text(encoding="utf-8")
start = source.index("static int sync_gameboy_verified_events(void)")
end = source.index("\nstatic char *process_tools_with_status", start)
block = source[start:end]

assert "gameboy_find_life_event_for_console_event(event_id)" in block, (
    "pending adapter events must reuse their existing Life Log event"
)
assert 'r2_log_event(\n                R2_LOG_WORLD, "gameboy_game_event"' in block, (
    "new adapter events must still be recorded in the Life Log"
)
assert "r2_reward_apply_once(" in block, (
    "replayed adapter events must not apply duplicate rewards"
)
assert 'snprintf(reward_source, sizeof(reward_source), "game_event:%lld"' in block, (
    "reward idempotency key must include the stable adapter event ID"
)
assert "if (reward_rc < 0)" in block and "leaving it pending" in block, (
    "failed reward persistence must not acknowledge and lose the event"
)
assert 'run_gameboy_console("ack_event", event_id_text' in block, (
    "event acknowledgement must happen only after durable Life Log/reward handling"
)
assert "gameboy_find_life_event_for_console_event" in source
helper_start = source.index("static int64_t gameboy_find_life_event_for_console_event")
helper_end = source.index("static int sync_gameboy_verified_events", helper_start)
helper = source[helper_start:helper_end]
assert "WHERE event_type='gameboy_game_event' AND details LIKE ?" in helper
assert "sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT)" in helper

print("Game Boy event idempotency smoke test passed.")
