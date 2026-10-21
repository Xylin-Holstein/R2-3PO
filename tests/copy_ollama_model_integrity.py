#!/usr/bin/env python3
"""Functional tests for integrity-checking copies of content-addressed Ollama blobs."""
from __future__ import annotations

import hashlib
import json
import os
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



    def test_incomplete_manifest_is_rejected_before_destination_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            manifest_path = source / copier.model_manifest_relative("qwen3.5:9b")
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

            # Missing layer digests must not be silently skipped: the copied
            # manifest would otherwise reference a blob that was never copied.
            manifest["layers"][0].pop("digest")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "manifest layer 0 is missing a blob digest"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertFalse(destination.exists())

            # A JSON value that is not an object must also fail cleanly, before
            # any destination tree is created.
            manifest_path.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid Ollama manifest object"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertFalse(destination.exists())

    def test_malformed_manifest_leaves_existing_destination_untouched(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            destination.mkdir()
            sentinel = destination / "keep-existing.txt"
            sentinel.write_text("preserve existing destination", encoding="utf-8")
            manifest_path = source / copier.model_manifest_relative("qwen3.5:9b")
            manifest_path.write_text(json.dumps({
                "config": {"digest": "sha256:" + hashlib.sha256(b"config").hexdigest()},
                "layers": [{"mediaType": "application/vnd.ollama.image.model"}],
            }), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "manifest layer 0 is missing a blob digest"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve existing destination")
            self.assertEqual(sorted(path.name for path in destination.iterdir()), ["keep-existing.txt"])

    def test_manifest_digest_path_syntax_is_rejected_before_destination_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            manifest_path = source / copier.model_manifest_relative("qwen3.5:9b")
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            traversal = "a/" + (".." + "/") * 4
            manifest["layers"][0]["digest"] = "sha256:" + traversal + ("b" * (64 - len(traversal)))
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "unsupported blob digest in manifest"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")

            self.assertFalse(destination.exists(), "invalid manifest digests must fail before creating destination files")

    def test_preplanted_predictable_temp_symlinks_are_not_followed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            copier.copy_model(source, destination, "qwen3.5:9b", "")
            weights_digest = hashlib.sha256(b"weights").hexdigest()
            copied_blob = destination / "blobs" / ("sha256-" + weights_digest)
            copied_blob.write_bytes(b"WEIGHT!")  # Force the blob-repair path.

            source_manifest = source / copier.model_manifest_relative("qwen3.5:9b")
            copied_manifest = destination / copier.model_manifest_relative("qwen3.5:9b")
            outside_blob = base / "outside-blob"
            outside_manifest = base / "outside-manifest"
            outside_blob.write_bytes(b"blob sentinel")
            outside_manifest.write_bytes(b"manifest sentinel")

            # Older PID-derived temp paths are attacker-predictable. Plant links
            # at both locations and verify the copier never writes through them.
            blob_temp = copied_blob.with_name(copied_blob.name + f".tmp.{os.getpid()}")
            manifest_temp = copied_manifest.with_name(copied_manifest.name + f".tmp.{os.getpid()}")
            blob_temp.symlink_to(outside_blob)
            manifest_temp.symlink_to(outside_manifest)

            copier.copy_model(source, destination, "qwen3.5:9b", "")

            self.assertEqual(outside_blob.read_bytes(), b"blob sentinel")
            self.assertEqual(outside_manifest.read_bytes(), b"manifest sentinel")
            self.assertTrue(blob_temp.is_symlink())
            self.assertTrue(manifest_temp.is_symlink())
            self.assertEqual(copied_blob.read_bytes(), b"weights")
            self.assertEqual(copied_manifest.read_bytes(), source_manifest.read_bytes())

    def test_symlinked_source_manifest_is_rejected_before_destination_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            manifest_path = source / copier.model_manifest_relative("qwen3.5:9b")
            external_manifest = base / "external-manifest"
            external_manifest.write_bytes(manifest_path.read_bytes())
            manifest_path.unlink()
            manifest_path.symlink_to(external_manifest)

            with self.assertRaisesRegex(ValueError, "refusing symlinked source manifest"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertFalse(destination.exists())

    def test_symlinked_source_blob_is_rejected_before_destination_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r2-model-copy-test-") as temporary:
            base = Path(temporary)
            source, destination = base / "source", base / "destination"
            self._write_model(source, "qwen3.5:9b", {b"config": "unused", b"weights": "unused"})
            weights_digest = hashlib.sha256(b"weights").hexdigest()
            source_blob = source / "blobs" / ("sha256-" + weights_digest)
            external_blob = base / "external-blob"
            external_blob.write_bytes(b"weights")
            source_blob.unlink()
            source_blob.symlink_to(external_blob)

            with self.assertRaisesRegex(ValueError, "refusing symlinked source model blob"):
                copier.copy_model(source, destination, "qwen3.5:9b", "")
            self.assertFalse(destination.exists())

    def test_model_reference_rejects_path_traversal_and_invalid_components(self) -> None:
        invalid = (
            "../outside:latest",
            "qwen3.5:../outside",
            "qwen3.5:tag/../../outside",
            "org/../../outside:latest",
            "org//model:latest",
            "model:",
            ":latest",
            "model:tag:extra",
        )
        for model in invalid:
            with self.subTest(model=model):
                with self.assertRaisesRegex(ValueError, "invalid Ollama model reference"):
                    copier.model_manifest_relative(model)

    def test_model_reference_accepts_normal_namespaced_and_tagged_models(self) -> None:
        self.assertEqual(
            copier.model_manifest_relative("qwen3.5:9b").as_posix(),
            "manifests/registry.ollama.ai/library/qwen3.5/9b",
        )
        self.assertEqual(
            copier.model_manifest_relative("my-org/model_name:latest-quantized").as_posix(),
            "manifests/registry.ollama.ai/my-org/model_name/latest-quantized",
        )

    def test_cli_rejects_invalid_reference_before_model_store_discovery(self) -> None:
        import contextlib
        import io
        from unittest.mock import patch
        errors = io.StringIO()
        with patch.object(sys, "argv", ["copy_ollama_model.py", "--model", "qwen3.5:../outside"]), \
             patch.object(copier, "service_model_root") as discover:
            with contextlib.redirect_stderr(errors):
                status = copier.main()
        self.assertEqual(status, 1)
        discover.assert_not_called()
        self.assertIn("invalid Ollama model reference", errors.getvalue())


    def test_cli_defaults_to_the_runtime_model(self) -> None:
        from unittest.mock import patch
        with tempfile.TemporaryDirectory(prefix="r2-model-default-test-") as temporary:
            source = Path(temporary)
            with patch.dict(copier.os.environ, {}, clear=True), \
                 patch.object(sys, "argv", ["copy_ollama_model.py"]), \
                 patch.object(copier, "service_model_root", return_value=source), \
                 patch.object(copier, "copy_model") as copy_model:
                status = copier.main()
        self.assertEqual(status, 0)
        self.assertEqual(copy_model.call_args.args[2], "qwen3.5:9b")

if __name__ == "__main__":
    unittest.main(verbosity=2)