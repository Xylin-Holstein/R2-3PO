import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "audit_r2_sound_assets.py"
SPEC = importlib.util.spec_from_file_location("audit_r2_sound_assets", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(audit)


class SoundAssetAuditTests(unittest.TestCase):
    def test_inventory_is_read_only_and_reports_kind_and_state_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = [
                "R2_Beep_Curious.MP3",
                "R2_Beep_Default.mp3",
                "R2_Whistle_Happy.mp3",
                "not_a_sound.wav",
            ]
            for name in names:
                (root / name).write_bytes(b"fixture-not-a-real-mp3")
            (root / "R2_Beep_Sleepy.mp3").mkdir()
            before = {p.name: (p.is_dir(), p.read_bytes() if p.is_file() else None)
                      for p in root.iterdir()}

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 0)

            report = output.getvalue()
            self.assertIn("Readable regular MP3 files: 3", report)
            self.assertIn("R2_Beep_Curious.MP3 [curious]", report)
            self.assertIn("Beep candidates: 2", report)
            self.assertIn("Whistle candidates: 1", report)
            self.assertIn("State curious: beep", report)
            self.assertIn("State happy: whistle", report)
            self.assertIn("does not decode MP3s", report)
            after = {p.name: (p.is_dir(), p.read_bytes() if p.is_file() else None)
                     for p in root.iterdir()}
            self.assertEqual(before, after)

    def test_missing_directory_is_reported_without_creating_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "missing"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 2)
            self.assertIn("missing or unreadable", output.getvalue())
            self.assertFalse(root.exists())


if __name__ == "__main__":
    unittest.main()
