#!/usr/bin/env python3
"""Regression test for Life Log declarations whose definitions must link."""

from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "Log.c").read_text(encoding="utf-8")
header = (root / "Log.h").read_text(encoding="utf-8")

required = {
    "r2_log_inner_state": r"int64_t\s+r2_log_inner_state\s*\(",
    "r2_log_belief": r"int64_t\s+r2_log_belief\s*\(",
    "r2_log_continuity": r"int64_t\s+r2_log_continuity\s*\(",
    "r2_log_link": r"int\s+r2_log_link\s*\(",
    "r2_log_hypothetical": r"int64_t\s+r2_log_hypothetical\s*\(",
    "r2_log_status_report": r"char\s*\*\s*r2_log_status_report\s*\(",
}
missing = []
for name, pattern in required.items():
    if not re.search(pattern, header):
        missing.append(f"header declaration missing: {name}")
    if not re.search(pattern, source):
        missing.append(f"Log.c implementation missing: {name}")

for table in (
    "r2_log_inner_states",
    "r2_log_beliefs",
    "r2_log_continuity",
    "r2_log_links",
    "r2_log_hypotheticals",
):
    if f"CREATE TABLE IF NOT EXISTS {table}" not in source:
        missing.append(f"schema missing: {table}")

if missing:
    print("\n".join(missing), file=sys.stderr)
    raise SystemExit(1)

print("Life Log structured API regression checks passed.")
