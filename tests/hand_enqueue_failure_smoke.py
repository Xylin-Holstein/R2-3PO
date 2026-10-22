#!/usr/bin/env python3
"""Regression check: never report a HAND task as started when enqueue failed."""
from pathlib import Path
import re

source = Path("r2.c").read_text(encoding="utf-8")
matches = list(re.finditer(r"char\s*\*id\s*=\s*start_background_task\s*\(", source))
assert len(matches) == 3, f"expected 3 background enqueue call sites, found {len(matches)}"

for match in matches:
    tail = source[match.end():match.end() + 1400]
    cleanup = re.search(r"free\(id\);", tail)
    assert cleanup, "could not find task enqueue result cleanup"
    block = tail[:cleanup.end()]
    assert re.search(r"if\s*\(\s*id\s*\)", block), (
        "task enqueue result is not checked before reporting success"
    )
    assert "The command passed validation, but the task could not be queued." in block, (
        "enqueue failure must be reported as a failure, not a successful task start"
    )

print("Background task enqueue failures are reported accurately at all three dispatch sites.")
