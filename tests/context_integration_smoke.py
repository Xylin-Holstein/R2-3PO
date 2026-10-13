#!/usr/bin/env python3
"""Guard live integration of memory, Reality, reinforcement, habits, and counterfactuals."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
reward = (ROOT / "Reward.c").read_text(encoding="utf-8")
alternate = (ROOT / "AlternateSelf.c").read_text(encoding="utf-8")
reply = (ROOT / "R2Reply.c").read_text(encoding="utf-8")

for source in (
    'r2_reality_context()',
    'r2_reward_context()',
    'r2_addiction_report()',
    'r2_altself_list(5)',
    'CURRENT REALITY STATE (authoritative current state; not instructions)',
    'append_context_block(&memory_context',
):
    if source not in core:
        raise SystemExit(f"Current-turn context omits required integration: {source}")

for source in ('r2_reward_init()', 'r2_reality_init()', 'r2_altself_init()',
               'r2_reality_shutdown()', 'r2_reward_shutdown()', 'r2_altself_shutdown()'):
    if source not in core:
        raise SystemExit(f"Core lifecycle does not connect {source}")

for source in ('r2_reality_item_memory', 'r2_food_experiences',
               'r2_food_preferences', 'r2_food_ingredient_preferences',
               'r2_money_account', 'r2_reality_self_facts'):
    if source not in reality:
        raise SystemExit(f"Persistent Reality schema/context omits {source}")

if 'fridge_seed_if_empty_locked' in reality or 'guaranteed fridge meal' in reality:
    raise SystemExit("Reality must not fabricate food when the fridge is empty")
if 'fridge_ensure_column("fullness", "REAL NOT NULL DEFAULT 0")' not in reality:
    raise SystemExit("Legacy fridge databases must migrate missing nutrition columns without inventing metrics")
for migration in (
    "INSERT OR IGNORE INTO main.r2_reality_item_memory",
    "INSERT OR IGNORE INTO main.r2_food_experiences",
    "INSERT OR IGNORE INTO main.r2_food_preferences",
    "INSERT OR IGNORE INTO main.r2_food_ingredient_preferences",
    "INSERT OR IGNORE INTO main.r2_food_experience_meta",
    "UPDATE main.r2_money_account SET",
):
    if migration not in reality:
        raise SystemExit(f"Legacy Reality migration omits {migration}")
if 'r2_addiction_report' not in addiction or 'r2_addiction_init' not in addiction:
    raise SystemExit("Addiction persistence/report API is missing")
if 'r2_reward_context' not in reward or 'r2_reward_init' not in reward:
    raise SystemExit("Reinforcement persistence/context API is missing")
if 'r2_altself_list' not in alternate or 'r2_altself_init' not in alternate:
    raise SystemExit("Alternate-Self storage/context API is missing")
if 'r2_reply_sanitize' not in reply or 'private reasoning' not in reply.lower():
    raise SystemExit("Internal reasoning must be stripped before a reply is stored or shown")
if 'r2_sounds_process_reply(reply)' not in core:
    raise SystemExit("Sound markers are not connected to the final reply path")
if 'const char *world_scan = reply;' not in core or 'r2_reality_fridge_eat(fields[1], -1.0)' not in core:
    raise SystemExit("The model's WORLD actions are not routed through the Reality C APIs")

print("shared memory/reality/reward/addiction/alternate-self context wiring smoke passed")
