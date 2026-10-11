#!/usr/bin/env python3
"""Ensure all documented R2 build paths use the same strict source/config policy."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")

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
    'gcc -std=c11 -Wall -Wextra -Werror -O2 \\' in launcher,
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
print("R2 build configuration smoke test passed.")
