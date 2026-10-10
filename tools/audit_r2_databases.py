#!/usr/bin/env python3
"""Read-only preflight audit for R2's SQLite databases.

This tool never initializes R2, creates tables, migrates schemas, or writes to a
source database. Point --root at the R2 code/data directory containing
r2_reality.db, r2_fridge.db, and r2_memory.db. For a live robot, stop R2 first
or audit a consistent copy of the database files.
"""
from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path
from typing import Any

DATABASES = ("r2_reality.db", "r2_fridge.db", "r2_memory.db")
LEGACY_PHANTOM_DESCRIPTION = (
    "Guaranteed filling burger generated because the fridge was empty"
)


def _readonly_connection(path: Path) -> sqlite3.Connection:
    # URI quoting is required for spaces, #, and ? in filesystem paths.
    uri = path.resolve().as_uri() + "?mode=ro"
    db = sqlite3.connect(uri, uri=True, timeout=2.0)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA query_only=ON")
    return db


def audit_database(path: Path) -> dict[str, Any]:
    report: dict[str, Any] = {
        "path": str(path),
        "exists": path.is_file(),
        "integrity": None,
        "tables": {},
        "legacy_phantom_food_records": [],
        "error": None,
    }
    if not report["exists"]:
        report["error"] = "database file not found"
        return report

    db: sqlite3.Connection | None = None
    try:
        db = _readonly_connection(path)
        integrity_rows = db.execute("PRAGMA integrity_check").fetchall()
        integrity = [str(row[0]) for row in integrity_rows]
        report["integrity"] = integrity
        table_rows = db.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        ).fetchall()

        for table_row in table_rows:
            table = str(table_row[0])
            # Table identifiers originate from sqlite_master, but still quote
            # embedded quotes before using the name in a count query.
            quoted = '"' + table.replace('"', '""') + '"'
            columns = [str(row[1]) for row in db.execute(f"PRAGMA table_info({quoted})")]
            count = int(db.execute(f"SELECT COUNT(*) FROM {quoted}").fetchone()[0])
            report["tables"][table] = {"rows": count, "columns": columns}

            if "description" in columns:
                count_row = db.execute(
                    f"SELECT COUNT(*) FROM {quoted} WHERE description = ?",
                    (LEGACY_PHANTOM_DESCRIPTION,),
                ).fetchone()
                matches = int(count_row[0])
                if matches:
                    report["legacy_phantom_food_records"].append(
                        {"table": table, "rows": matches}
                    )
    except sqlite3.Error as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if db is not None:
            db.close()
    return report


def audit_root(root: Path) -> dict[str, Any]:
    reports = {name: audit_database(root / name) for name in DATABASES}
    existing = [item for item in reports.values() if item["exists"]]
    failures = [
        name for name, item in reports.items()
        if item["exists"] and (
            item["error"] is not None or item["integrity"] != ["ok"]
        )
    ]
    missing = [name for name, item in reports.items() if not item["exists"]]
    legacy = [
        {"database": name, **record}
        for name, item in reports.items()
        for record in item["legacy_phantom_food_records"]
    ]
    return {
        "root": str(root.resolve()),
        "read_only": True,
        "databases": reports,
        "summary": {
            "found": len(existing),
            "expected": len(DATABASES),
            "missing": missing,
            "integrity_failures": failures,
            "legacy_phantom_food_records": legacy,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, required=True,
        help="R2 data directory containing the SQLite database files",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="emit machine-readable JSON instead of the human-readable summary",
    )
    args = parser.parse_args()
    report = audit_root(args.root)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"R2 database preflight (read-only): {report['root']}")
        for name, item in report["databases"].items():
            if not item["exists"]:
                print(f"  MISSING  {name}")
                continue
            integrity = ", ".join(item["integrity"] or []) or "unavailable"
            print(f"  {'ERROR' if item['error'] or integrity != 'ok' else 'OK'}  "
                  f"{name}: integrity={integrity}, tables={len(item['tables'])}")
            if item["error"]:
                print(f"           error: {item['error']}")
            for table, detail in item["tables"].items():
                print(f"           {table}: {detail['rows']} rows")
            for record in item["legacy_phantom_food_records"]:
                print(f"           WARNING: {record['rows']} legacy phantom-food "
                      f"row(s) in {record['table']}")
        summary = report["summary"]
        print(f"Summary: {summary['found']}/{summary['expected']} database files found; "
              f"{len(summary['integrity_failures'])} integrity failure(s); "
              f"{len(summary['legacy_phantom_food_records'])} legacy phantom-food "
              "record group(s).")
        if summary["missing"]:
            print("Note: missing files are reported, not created.")
        if summary["legacy_phantom_food_records"]:
            print("Note: suspicious legacy rows are reported only; no rows are changed.")

    # Missing databases are a warning because some deployments may not yet have
    # every subsystem. A present but unreadable/corrupt database is a failure.
    return 2 if report["summary"]["integrity_failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
