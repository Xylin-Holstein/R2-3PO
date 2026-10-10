# R2 database preflight

`tools/audit_r2_databases.py` inspects the SQLite files in R2's data directory:

- `r2_reality.db`
- `r2_fridge.db`
- `r2_memory.db`

It opens existing databases read-only, runs SQLite's `integrity_check`, reports user tables/row counts, and flags exact matches for the legacy phantom-burger description. It does not repair databases, initialize missing databases, migrate schemas, or delete flagged records.

## Audit a stopped R2 instance

Pass the directory that contains the database files (normally `/home/x/R2_Home/R2`):

```sh
python3 tools/audit_r2_databases.py --root /home/x/R2_Home/R2
```

Add `--json` for machine-readable output.

## Snapshot and audit

The script can use SQLite's backup API to create a per-database snapshot and audit the copies:

```sh
python3 tools/audit_r2_databases.py \
  --root /home/x/R2_Home/R2 \
  --snapshot-dir /home/x/R2_Home/database-preflight-snapshot
```

Existing snapshot files are never overwritten. Choose a new or empty destination directory. If any expected snapshot filename already exists, the script refuses the whole snapshot set before copying anything, so it cannot silently mix stale and fresh database copies. New snapshot files are created with owner-only permissions (0600). Source database files are opened read-only; suspicious rows are reported, not altered.

Snapshots are consistent individually, but the three independent databases are not captured at one atomic instant. Stop R2 before running the audit when cross-database consistency matters. Keep the snapshot directory private because these databases may contain personal memories and activity records. Do not upload the database files to public issues or logs.

A missing database is reported as missing and is not created. Because this is a deployment preflight for the three expected operational databases, any missing database, unreadable/corrupt database, integrity failure, or snapshot error yields a non-zero exit status. If you intentionally run a partial installation, treat the missing-database report as an expected limitation rather than allowing the preflight to pass silently.
