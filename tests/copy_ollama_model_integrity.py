#!/usr/bin/env python3
"""Functional tests for integrity-checking copies of content-addressed Ollama blobs."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import copy_ollama_model as copier  # noqa: E402


class CopyOllamaModelIntegrityTests(unittest.TestCase):
    def _write_model(self, root: Path, model: str, blobs: dict[bytes, str]) -> dict[bytes, Path]:
        blob_paths: dict[bytes, Path] = {}
        for payload in blobs:
            digest = hashlib.sha256(payload).hexdigest()
            path = root / "blobs" / ("sha256-" + digest)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            blob_paths[payload] = path
        manifest_path = root / copier.model_manifest_relative(model)
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest = {
            "schemaVersion": 2,
            "config": {"digest": "sha256:" + hashlib.sha256(b"config").hexdigest()},
            "layers": [{"digest": "sha256:" + hashlib.sha256(b"weights").hexdigest()}],
        }
        (root / "blobs" / ("sha256-" + hashlib.sha256(b"config").hexdigest())).write_bytes(b"config")
        (root / "blobs" / ("sha256-" + hashlib.sha256(b"weights").hexdigest())).write_bytes(b"weights")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        return blob_paths

    def test_same_size_corrupt_destination_is_repaired(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            weights_digest = hashlib.sha256(b"weights").hexdigest()
            copied = destination / "blobs" / ("sha256-" + weights_digest)
            copied.write_bytes(b"WEIGHT!")  # Same seven-byte size, wrong content.
            self.assertEqual(copied.stat().st_size, len(b"weights"))
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertEqual(copied.read_bytes(), b"weights")

    def test_symlinked_destination_blob_is_replaced_not_followed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            weights_digest = hashlib.sha256(b"weights").hexdigest()
            copied = destination / "blobs" / ("sha256-" + weights_digest)
            outside = base / "outside-sentinel"
            outside.write_bytes(b"weights")
            copied.unlink()
            copied.symlink_to(outside)
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertFalse(copied.is_symlink(), "destination blob symlink must be replaced")
            self.assertEqual(copied.read_bytes(), b"weights")
            self.assertEqual(outside.read_bytes(), b"weights", "external target must remain untouched")

    def test_symlinked_blob_directory_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            external = base / "external-blobs"
            external.mkdir()
            sentinel = external / "sentinel"
            sentinel.write_text("must remain untouched", encoding="utf-8")
            import shutil
            shutil.rmtree(destination / "blobs")
            (destination / "blobs").symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "refusing symlinked destination path component"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "must remain untouched")

    def test_corrupt_source_blob_is_rejected_before_overwrite(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            weights_digest = hashlib.sha256(b"weights").hexdigest()
            source_blob = source / "blobs" / ("sha256-" + weights_digest)
            destination_blob = destination / "blobs" / ("sha256-" + weights_digest)
            source_blob.write_bytes(b"WEIGHT!")
            with self.assertRaisesRegex(ValueError, "source model blob failed SHA-256 verification"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertEqual(destination_blob.read_bytes(), b"weights")


if __name__ == "__main__":
    unittest.main(verbosity=2)
