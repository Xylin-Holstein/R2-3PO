#!/usr/bin/env python3
"""Source-contract checks for timed hunger progression and single-turn feeding."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
header = (ROOT / "Reality.h").read_text(encoding="utf-8")

worker_start = core.index("static void *reality_needs_worker(void *unused)")
worker_end = core.index("\nint r2_init(void)", worker_start)
worker = core[worker_start:worker_end]
assert "pthread_mutex_trylock(&conversation_turn_lock)" in worker
assert "r2_reality_autonomous_feed_if_needed()" in worker
assert "pthread_mutex_unlock(&conversation_turn_lock)" in worker
assert "nanosleep(&delay, &delay)" in worker
assert "pthread_join(reality_needs_thread, NULL)" in core

talk_start = core.index("char *r2_talk(const char *message)\n{")
talk_end = core.index("\nint r2_conversation(void)", talk_start)
talk = core[talk_start:talk_end]
assert talk.index("pthread_mutex_lock(&conversation_turn_lock)") < talk.index("r2_talk_impl(message)")
assert "pthread_mutex_unlock(&conversation_turn_lock)" in talk

feed_start = reality.index("int r2_reality_autonomous_feed_if_needed(void)")
feed_end = reality.index("\nint r2_reality_autonomous_food_available(void)", feed_start)
feed = reality[feed_start:feed_end]
assert "if (hunger < 50.0)" in feed
assert "food_metric((const char *)name, &candidate_fullness" in feed
assert "candidate_fullness > 0.0" in feed
assert "return 1;" in feed
assert "r2_reality_fridge_eat(food, -1.0)" in feed
assert "if (hunger >= 50.0)" not in feed

availability_start = reality.index("int r2_reality_autonomous_food_available(void)")
availability_end = reality.index("\nchar *r2_reality_food_context", availability_start)
availability = reality[availability_start:availability_end]
assert "fullness>0" in availability
assert "return rc == SQLITE_OK ? available : -1;" in availability

assert "fridge_seed_if_empty_locked" not in reality
assert "guaranteed fridge meal" not in reality
assert "when hunger reaches 50/100 or higher" in header
assert 'const char *world_scan = reply;' in core
assert 'r2_reality_eat(fields[1], -1.0)' in core
assert 'r2_reality_fridge_eat(fields[1], -1.0)' in core
print("autonomous needs worker and single-meal cycle smoke passed")
