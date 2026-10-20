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

activity_start = source[source.index("int64_t r2_log_activity_start("):source.index("int64_t r2_log_activity_end(")]
if "int step_rc = sqlite3_step(statement);" not in activity_start:
    missing.append("activity-start lookup does not preserve SQLite step errors")
for required_status in ("step_rc == SQLITE_ROW", "step_rc == SQLITE_DONE", "rc = step_rc;"):
    if required_status not in activity_start:
        missing.append(f"activity-start lookup missing status branch: {required_status}")
if "char summary[1024];" in activity_start:
    missing.append("activity-start insert retains unused summary allocation")

query_start = source.index("static char *query_events(")
query_end = source.index("char *r2_log_recent(", query_start)
query_impl = source[query_start:query_end]
for required_text in ("FROM r2_log_links l JOIN r2_log_events", "l.relationship", "l.notes", "Linked event", "event_id_seen"):
    if required_text not in query_impl:
        missing.append(f"link-aware search missing: {required_text}")

print("Life Log structured API regression checks passed.")
