import importlib.util
import sqlite3
import stat
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "audit_r2_databases.py"
SPEC = importlib.util.spec_from_file_location("audit_r2_databases", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(audit)


class DatabasePreflightTests(unittest.TestCase):
    def test_audit_is_read_only_and_finds_legacy_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "r2_fridge.db"
            db = sqlite3.connect(path)
            db.execute(
                "CREATE TABLE r2_fridge_items "
                "(name TEXT, description TEXT, quantity INTEGER)"
            )
            db.execute(
                "INSERT INTO r2_fridge_items VALUES (?, ?, ?)",
                ("burger", audit.LEGACY_PHANTOM_DESCRIPTION, 1),
            )
            db.commit()
            db.close()
            before = path.read_bytes()

            result = audit.audit_database(path)

            self.assertEqual(result["integrity"], ["ok"])
            self.assertEqual(
                result["legacy_phantom_food_records"],
                [{"table": "r2_fridge_items", "rows": 1}],
            )
            self.assertEqual(path.read_bytes(), before)
            db = sqlite3.connect(path)
            self.assertEqual(
                db.execute("SELECT quantity FROM r2_fridge_items").fetchone()[0], 1
            )
            db.close()

    def test_snapshot_is_consistent_and_never_overwrites_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "r2_fridge.db"
            snapshot = root / "snapshots" / "r2_fridge.db"
            db = sqlite3.connect(source)
            db.execute("CREATE TABLE stock (name TEXT, quantity INTEGER)")
            db.execute("INSERT INTO stock VALUES ('apple', 3)")
            db.commit()
            db.close()
            original = source.read_bytes()
            snapshot.parent.mkdir()

            audit.snapshot_database(source, snapshot)

            self.assertEqual(source.read_bytes(), original)
            copied = sqlite3.connect(snapshot)
            self.assertEqual(copied.execute("SELECT quantity FROM stock").fetchone()[0], 3)
            copied.close()
            with self.assertRaises(FileExistsError):
                audit.snapshot_database(source, snapshot)

    def test_snapshot_is_private_and_existing_set_is_refused_before_partial_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            snapshot_dir = Path(directory) / "snapshots"
            root.mkdir()
            source = root / "r2_reality.db"
            db = sqlite3.connect(source)
            db.execute("CREATE TABLE safe (value TEXT)")
            db.execute("INSERT INTO safe VALUES ('source')")
            db.commit()
            db.close()

            snapshot_dir.mkdir()
            stale = snapshot_dir / "r2_fridge.db"
            stale.write_text("preserve me", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                audit.create_snapshot_set(root, snapshot_dir)

            self.assertFalse((snapshot_dir / "r2_reality.db").exists())
            self.assertEqual(stale.read_text(encoding="utf-8"), "preserve me")

            stale.unlink()
            created = audit.create_snapshot_set(root, snapshot_dir)
            self.assertEqual(created, [snapshot_dir / "r2_reality.db"])
            self.assertEqual(stat.S_IMODE(created[0].stat().st_mode) & 0o077, 0)
            snapshot = sqlite3.connect(created[0])
            self.assertEqual(snapshot.execute("SELECT value FROM safe").fetchone()[0], "source")
            snapshot.close()

    def test_empty_database_is_valid_and_missing_files_are_not_created(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            db_path = root / "r2_reality.db"
            sqlite3.connect(db_path).close()

            report = audit.audit_root(root)

            self.assertEqual(report["databases"]["r2_reality.db"]["integrity"], ["ok"])
            self.assertIn("r2_fridge.db", report["summary"]["missing"])
            self.assertFalse((root / "r2_fridge.db").exists())
            self.assertFalse((root / "r2_memory.db").exists())

    def test_corrupt_or_non_sqlite_file_is_reported_as_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "r2_memory.db"
            path.write_bytes(b"this is not a SQLite database")

            report = audit.audit_database(path)

            self.assertTrue(report["exists"])
            self.assertIsNotNone(report["error"])


if __name__ == "__main__":
    unittest.main()
