#!/usr/bin/env python3
"""Regression checks for hunger thresholds and coordination with foreground turns."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

worker_start = core.find("static void *reality_needs_worker(void *unused)")
worker_end = core.find("\nstatic int configure_ollama_endpoint", worker_start)
if worker_start < 0 or worker_end < 0:
    raise SystemExit("Could not locate the autonomous Reality needs worker.")
worker = core[worker_start:worker_end]
if "atomic_load(&foreground_conversation_waiting) == 0" not in worker:
    raise SystemExit("Autonomous feeding must yield while a foreground conversation is waiting.")
if "r2_reality_autonomous_feed_if_needed()" not in worker:
    raise SystemExit("The needs worker must continue using the authoritative Reality feeding API.")

feed_start = reality.find("int r2_reality_autonomous_feed_if_needed(void)")
feed_end = reality.find("\nint r2_reality_autonomous_food_available(void)", feed_start)
if feed_start < 0 or feed_end < 0:
    raise SystemExit("Could not locate the persistent autonomous feeding implementation.")
feed = reality[feed_start:feed_end]
if "if (hunger < 50.0)" not in feed:
    raise SystemExit("The increasing hunger meter must trigger feeding at 50/100 or higher.")
if "if (hunger >= 50.0)" in feed:
    raise SystemExit("Autonomous feeding still uses the reversed fullness threshold and can eat while not hungry.")
if "food_metric((const char *)name, &candidate_fullness, NULL) == 0" not in feed:
    raise SystemExit("Autonomous feeding must require a configured food metric for tracked inventory.")
if "candidate_fullness > 0.0" not in feed:
    raise SystemExit("Autonomous feeding must reject unknown/zero-fullness stock.")
if 'fullness>0 ORDER BY name LIMIT 1' not in feed:
    raise SystemExit("Autonomous feeding must not consume fridge stock with unknown/zero fullness.")

world_start = core.find("static char *world_action_result(char *line)")
world_end = core.find("\nstatic char *process_tools(", world_start)
if world_start < 0 or world_end < 0:
    raise SystemExit("Could not locate the shared WORLD action dispatcher.")
world = core[world_start:world_end]
for marker in (
    'if (!strcasecmp(f[0], "eat")',
    'r2_reality_eat(f[1], fullness)',
    'if (!strcasecmp(f[0], "fridge_eat")',
    'r2_reality_fridge_eat(f[1], -1.0)',
    'if (!strcasecmp(f[0], "sleep")',
):
    if marker not in world:
        raise SystemExit(f"WORLD action dispatcher is missing {marker}")
if "atof(" in world:
    raise SystemExit("WORLD action numbers must be validated, not parsed with unchecked atof().")
if "fridge_seed_if_empty_locked" in reality or "guaranteed fridge meal" in reality:
    raise SystemExit("Autonomous needs must never create synthetic food to satisfy hunger.")

talk_start = core.find("char *r2_talk(const char *message)\n{")
talk_end = core.find("\nint r2_conversation(void)", talk_start)
if talk_start < 0 or talk_end < 0:
    raise SystemExit("Could not locate the foreground conversation wrapper.")
talk = core[talk_start:talk_end]
if "atomic_fetch_add(&foreground_conversation_waiting, 1)" not in talk:
    raise SystemExit("Foreground turns must announce themselves before waiting for the turn lock.")
if "atomic_fetch_sub(&foreground_conversation_waiting, 1)" not in talk:
    raise SystemExit("Foreground waiting state must always be released after a turn.")
print("Autonomous hunger threshold, stock integrity, and foreground coordination checks passed.")
