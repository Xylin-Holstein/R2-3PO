#!/usr/bin/env python3
"""Ensure the latest core, launcher, and Makefile compile the same integrated modules."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

required_sources = (
    "r2.c", "shell.c", "r2_diary.c", "Log.c", "Reality.c", "Addiction.c",
    "Reward.c", "AlternateSelf.c", "Visual.c", "Ears.c", "Eyes.c",
    "R2Sounds.c", "R2Reply.c",
)
require(re.search(r"^CFLAGS\s*\?=\s*-std=c11\s+-Wall\s+-Wextra\s+-Werror\s+-O2\s*$", makefile, re.M) is not None,
        "Makefile uses strict warning-as-error compilation")
require('gcc -std=c11 -Wall -Wextra -Werror -O2 \\' in launcher,
        "interactive launcher uses the same strict compiler flags")
require('TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"' in launcher and
        '-o "$TEMP_EXEC" \\' in launcher and
        'mv -f "$TEMP_EXEC" "$R2_EXEC"' in launcher and
        '-o "$R2_EXEC" \\' not in launcher,
        "failed compilation cannot overwrite the previous executable")
require('make clean all CFLAGS="-std=c11 -Wall -Wextra -Werror -O2"' in workflow,
        "CI compiles the production source list with strict flags")
source_block = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.M)
require(source_block is not None, "Makefile declares production sources")
for source in required_sources:
    require(source in source_block.group(1), f"Makefile includes {source}")
    require(f'"$R2_SOURCE/{source}"' in launcher, f"launcher includes {source}")
require('static const double HUNGER_PER_SECOND = 0.1 / 4.75;' in reality,
        "modeled hunger progresses at the configured 0.1-per-4.75-second rate")
require('if (hunger < 50.0) {' in reality and 'r2_reality_autonomous_feed_if_needed(void)' in reality,
        "autonomous feeding triggers at the hunger threshold rather than eating twice at low hunger")
require('return reality_eat_internal(food, -1.0, 1, -1.0);' in reality,
        "caller-supplied fullness cannot override configured food metrics")
require('food_metric(food, &fullness, &energy_bonus)' in reality and 'fridge_seed_if_empty_locked' not in reality,
        "food must have configured nutrition and empty fridge stock is never fabricated")
require('configure_ollama_endpoint()' in core and 'r2_reality_init()' in core,
        "the C core connects to the private Ollama endpoint and persistent Reality engine")
print("R2 build configuration smoke test passed.")
