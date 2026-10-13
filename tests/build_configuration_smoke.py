#!/usr/bin/env python3
"""Ensure all documented R2 build paths use the same strict source/config policy."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

required_sources = (
    "r2.c", "shell.c", "r2_diary.c", "Log.c", "Reality.c", "Addiction.c",
    "Reward.c", "AlternateSelf.c", "Visual.c", "Ears.c", "Eyes.c",
    "R2Sounds.c", "R2Reply.c",
)

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require(
    re.search(r"^CFLAGS\s*\?=\s*-std=c11\s+-Wall\s+-Wextra\s+-Werror\s+-O2\s*$",
              makefile, re.MULTILINE) is not None,
    "default Makefile build treats compiler warnings as errors",
)
require(
    'sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2 \\' in launcher,
    "interactive launcher uses the same strict compiler policy",
)
require(
    'TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"' in launcher
    and '-o "$TEMP_EXEC" \\' in launcher
    and 'mv -f "$TEMP_EXEC" "$R2_EXEC"' in launcher
    and '-o "$R2_EXEC" \\' not in launcher,
    "interactive build preserves the last known-good executable until atomic replacement",
)
require(
    'make clean all CFLAGS="-std=c11 -Wall -Wextra -Werror -O2"' in workflow,
    "CI compiles with the same strict compiler policy",
)
source_block = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.MULTILINE)
require(source_block is not None, "Makefile declares its production source list")
for source in required_sources:
    require(source in source_block.group(1), f"Makefile includes {source}")
    require(f'"$R2_SOURCE/{source}"' in launcher, f"launcher includes {source}")
require(
    "static const double HUNGER_PER_SECOND = 0.1 / 4.75;" in reality
    and "hunger += elapsed * HUNGER_PER_SECOND;" in reality
    and "satisfaction -= elapsed * HUNGER_PER_SECOND;" in reality
    and "sleepiness += elapsed * HUNGER_PER_SECOND;" in reality
    and "hunger > 100.0" in reality,
    "elapsed world time advances hunger, satiety, sleepiness, and energy at the configured rate",
)
require(
    "reality_needs_worker" in core
    and "r2_reality_autonomous_feed_if_needed()" in core
    and 'r2_reality_eat(trim(e), -1.0)' in core
    and 'r2_reality_fridge_eat(trim(e), -1.0)' in core,
    "background needs ticking and C-backed eating use verified inventory/fridge operations",
)
require(
    "legacy_copy_table" in reality
    and "r2_reality_item_memory" in reality
    and "r2_food_experiences" in reality
    and "r2_food_preferences" in reality
    and "r2_money_account" in reality
    and "r2_tv_state" in reality
    and "r2_tv_vcr_state" in reality
    and "legacy_reality_migrated" in reality,
    "legacy Reality migration preserves item memory, food history/preferences, money, TV, VCR, and self state",
)
require(
    "fridge_seed_if_empty_locked" not in reality
    and "guaranteed fridge meal" not in reality
    and 'name=\\"burger\\" fullness=\\"100\\"' not in reality,
    "an empty fridge stays empty; food and nutrition are never synthesized as a hardcoded burger",
)
print("R2 build configuration smoke test passed.")
