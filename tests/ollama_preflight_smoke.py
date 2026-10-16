#!/usr/bin/env python3
"""Unit smoke tests for the local Ollama readiness probe; never opens a socket."""
from __future__ import annotations

import contextlib
import io
import sys
import unittest
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import preflight_ollama_model as probe  # noqa: E402


class OllamaPreflightSmoke(unittest.TestCase):
    def test_model_and_real_chat_completion_are_required(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "R2_READY"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]
        gpu_result = SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )
        with patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=gpu_result) as gpu:
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 3)
        self.assertTrue(read.call_args_list[0].args[0].endswith("/api/tags"))
        self.assertTrue(read.call_args_list[1].args[0].endswith("/api/chat"))
        payload = read.call_args_list[1].kwargs["payload"]
        self.assertEqual(payload["model"], "qwen3.5:9b")
        self.assertIs(payload["think"], False)
        self.assertEqual(payload["options"]["num_predict"], 128)
        self.assertTrue(read.call_args_list[2].args[0].endswith("/api/ps"))
        self.assertEqual(gpu.call_count, 1)

    def test_configured_context_is_used_for_the_real_chat_probe(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "R2_READY"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_CTX": "8192"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(returncode=1, stdout="", stderr="")):
            self.assertTrue(probe.preflight(timeout=1))
        payload = read.call_args_list[1].kwargs["payload"]
        self.assertEqual(payload["options"]["num_ctx"], 8192)

    def test_invalid_context_override_is_ignored_by_preflight(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "R2_READY"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_CTX": "not-a-number"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(returncode=1, stdout="", stderr="")):
            self.assertTrue(probe.preflight(timeout=1))
        payload = read.call_args_list[1].kwargs["payload"]
        self.assertNotIn("num_ctx", payload["options"])

    def test_overnight_endpoint_is_used_for_tags_chat_and_process_checks(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "R2_READY"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]
        gpu_result = SimpleNamespace(returncode=1, stdout="", stderr="")
        with patch.dict(probe.os.environ, {"R2_OLLAMA_BASE_URL": "http://127.0.0.1:11435"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=gpu_result):
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(
            [call.args[0] for call in read.call_args_list],
            [
                "http://127.0.0.1:11435/api/tags",
                "http://127.0.0.1:11435/api/chat",
                "http://127.0.0.1:11435/api/ps",
            ],
        )

    def test_unapproved_non_loopback_endpoint_is_rejected(self) -> None:
        errors = io.StringIO()
        with patch.dict(probe.os.environ, {"R2_OLLAMA_BASE_URL": "http://example.com:11435"}), \
             patch.object(probe, "_read_json") as read:
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(timeout=1))
        read.assert_not_called()
        self.assertIn("must be http://127.0.0.1:11434 or", errors.getvalue())

    def test_gpu_diagnostics_failure_does_not_block_ready_model(self) -> None:
        # GPU placement is diagnostic only; a successful real chat must remain
        # ready if /api/ps or nvidia-smi is unavailable.
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "R2_READY"}},
            OSError("simulated /api/ps unavailable"),
        ]), patch.object(probe.subprocess, "run", side_effect=FileNotFoundError("nvidia-smi")):
            with contextlib.redirect_stderr(errors):
                self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(errors.getvalue(), "")

    def test_missing_model_fails_without_waiting_or_network(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", return_value={"models": []}) as read:
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertEqual(read.call_count, 1)
        self.assertIn("required local model qwen3.5:9b is not installed", errors.getvalue())

    def test_listed_model_without_chat_text_fails(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"message": {"content": "   "}},
        ]):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("chat generation produced no usable final answer", errors.getvalue())

    def test_api_failure_is_reported_without_network_access(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=OSError("simulated offline API")):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertIn("simulated offline API", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
