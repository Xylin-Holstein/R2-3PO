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
require(
    "      - 'release/**'" in workflow,
    "release branches run the same full CI workflow instead of silently skipping validation",
)
require(
    "python3 -m compileall -q ." in workflow,
    "CI syntax-checks every Python source and test module before targeted tests run",
)
require(
    'ROOT="$(mktemp -d /tmp/r2-log-linked-retrieval.' in workflow
    and '"$ROOT/log-linked-retrieval-test"' in workflow
    and 'sudo chmod -R a+rwx /home/x/R2_Home' not in workflow
    and 'sudo rm -rf /home/x/R2_Home' not in workflow,
    "Life Log integration test uses an isolated temporary root and never chmods/deletes a shared home path",
)
require(
    "python3 tests/copy_ollama_model_integrity.py" in workflow,
    "CI exercises repair of same-size corrupt model blobs and rejection of corrupt source blobs",
)
source_block = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.MULTILINE)
require(source_block is not None, "Makefile declares its production source list")
for source in required_sources:
    require(source in source_block.group(1), f"Makefile includes {source}")
    require(f'"$R2_SOURCE/{source}"' in launcher, f"launcher includes {source}")
require(
    "static const double HUNGER_PER_SECOND = 100.0 / (3.0 * 3600.0);" in reality
    and "double hunger = sqlite3_column_double(st, 0) - elapsed * HUNGER_PER_SECOND;" in reality
    and "hunger=MIN(100,hunger+?)" in reality,
    "fullness meter drains over three hours and eating restores it",
)
require(
    "static const double SATIETY_PER_SECOND = 0.1 / 4.75;" in reality
    and "double satisfaction = sqlite3_column_double(st, 4) - elapsed * SATIETY_PER_SECOND;" in reality,
    "satisfaction independently decays by 0.1 per 4.75 seconds",
)
require(
    "return valid_hunger && hunger < 50.0;" in core
    and "if (hunger < 50.0) {" in core
    and "If the fullness meter is below 50/100" in core,
    "model and autonomous feeding thresholds match decreasing fullness semantics",
)
require(
    "hunger_fullness_semantics_v2" in reality
    and "100-hunger" in reality,
    "legacy hunger values migrate to the new fullness scale exactly once",
)
print("R2 build configuration smoke test passed.")
