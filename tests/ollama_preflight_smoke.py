#!/usr/bin/env python3
"""Unit smoke tests for the local Ollama readiness probe; never opens a socket."""
from __future__ import annotations

import base64
import contextlib
import io
import json
import sys
import unittest
import urllib.error
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
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]
        gpu_result = SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )
        with patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=gpu_result) as gpu:
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 4)
        self.assertTrue(read.call_args_list[0].args[0].endswith("/api/tags"))
        self.assertTrue(read.call_args_list[1].args[0].endswith("/api/chat"))
        payload = read.call_args_list[1].kwargs["payload"]
        self.assertEqual(payload["model"], "qwen3.5:9b")
        self.assertIs(payload["think"], True)
        self.assertEqual(payload["options"]["num_predict"], 512)
        self.assertEqual(payload["options"]["temperature"], 1.0)
        self.assertEqual(payload["options"]["top_p"], 0.95)
        self.assertEqual(payload["options"]["top_k"], 20)
        self.assertEqual(payload["options"]["min_p"], 0.0)
        self.assertEqual(payload["options"]["presence_penalty"], 1.5)
        self.assertEqual(payload["options"]["repeat_penalty"], 1.0)
        self.assertTrue(read.call_args_list[2].args[0].endswith("/api/chat"))
        vision_payload = read.call_args_list[2].kwargs["payload"]
        png_bytes = base64.b64decode(vision_payload["messages"][1]["images"][0])
        self.assertEqual(png_bytes[:8], bytes((137, 80, 78, 71, 13, 10, 26, 10)))
        self.assertEqual(int.from_bytes(png_bytes[16:20], "big"), 640)
        self.assertEqual(int.from_bytes(png_bytes[20:24], "big"), 480)
        self.assertEqual(vision_payload["format"], "json")
        self.assertEqual(vision_payload["messages"][0]["role"], "system")
        self.assertEqual(vision_payload["options"]["num_predict"], 256)
        self.assertEqual(vision_payload["options"]["temperature"], payload["options"]["temperature"])
        self.assertTrue(read.call_args_list[3].args[0].endswith("/api/ps"))
        self.assertEqual(gpu.call_count, 1)

    def test_explicit_gpu_and_context_policy_reaches_real_chat_request(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3.5:9b", "size": 500000, "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {
            "R2_OLLAMA_NUM_GPU": "20",
            "R2_OLLAMA_NUM_CTX": "8192",
        }), patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 123 MiB", stderr=""
             )):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        options = read.call_args_list[1].kwargs["payload"]["options"]
        self.assertEqual(options["num_gpu"], 20)
        self.assertEqual(options["num_ctx"], 8192)
        vision_payload = read.call_args_list[2].kwargs["payload"]
        self.assertEqual(vision_payload["options"]["num_gpu"], 20)
        self.assertEqual(vision_payload["options"]["num_ctx"], 8192)
        self.assertEqual(vision_payload["options"]["num_predict"], 512)
        self.assertIn("images", vision_payload["messages"][1])

    def test_overnight_endpoint_is_used_for_tags_chat_and_process_checks(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
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
                "http://127.0.0.1:11435/api/chat",
                "http://127.0.0.1:11435/api/ps",
            ],
        )

    def test_required_gpu_offload_rejects_cpu_only_success(self) -> None:
        errors = io.StringIO()
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "20"}), \
             patch.object(probe, "_read_json", side_effect=[
                 {"models": [{"name": "qwen3.5:9b"}]},
                 {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
                 {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
                 {"models": [{"name": "qwen3.5:9b", "size_vram": 0}]},
             ]), patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 0 MiB", stderr=""
             )), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertIn("mixed CPU/GPU placement was required", errors.getvalue())

    def test_automatic_layer_placement_passes_only_with_verified_mixed_use(self) -> None:
        replies = [
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3.5:9b", "size": 500000, "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "0", "R2_OLLAMA_NUM_CTX": "8192"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 128 MiB", stderr=""
             )):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertNotIn("num_gpu", read.call_args_list[1].kwargs["payload"]["options"])
        self.assertEqual(read.call_args_list[2].kwargs["payload"]["options"]["num_predict"], 512)

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
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            OSError("simulated /api/ps unavailable"),
        ]), patch.object(probe.subprocess, "run", side_effect=FileNotFoundError("nvidia-smi")):
            with contextlib.redirect_stderr(errors):
                self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(errors.getvalue(), "")

    def test_wrong_color_json_does_not_block_typed_chat_ready_status(self) -> None:
        errors = io.StringIO()
        red_literal = json.dumps({"color": "red"})
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": red_literal}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]) as read, patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )), contextlib.redirect_stderr(errors):
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 4)
        self.assertIn("failed the JSON known-color vision probe", errors.getvalue())

    def test_image_inference_failure_does_not_block_typed_chat_with_verified_gpu(self) -> None:
        errors = io.StringIO()
        output = io.StringIO()
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "20"}), \
             patch.object(probe, "_read_json", side_effect=[
                 {"models": [{"name": "qwen3.5:9b"}]},
                 {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
                 OSError("simulated CUDA out of memory during image inference"),
                 {"models": [{"name": "qwen3.5:9b", "size": 500000, "size_vram": 123456}]},
             ]) as read, patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
             )), contextlib.redirect_stderr(errors), contextlib.redirect_stdout(output):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertEqual(read.call_count, 4)
        self.assertIn("image-bearing chat failed", errors.getvalue())
        self.assertIn("CUDA out of memory", errors.getvalue())
        self.assertIn("text chat is ready", output.getvalue())
        self.assertIn("vision probe failed", output.getvalue())

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

    def test_incomplete_text_turn_does_not_pass_preflight(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": False, "message": {"role": "assistant", "content": "R2_READY"}},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 2)
        self.assertIn("did not return a complete assistant turn", errors.getvalue())

    def test_incomplete_vision_turn_does_not_block_typed_chat(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": False, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 4)
        self.assertIn("incomplete or invalid assistant turn", errors.getvalue())

    def test_http_500_body_is_preserved_for_text_generation(self) -> None:
        errors = io.StringIO()
        failure = urllib.error.HTTPError(
            "http://127.0.0.1:11434/api/chat",
            500,
            "Internal Server Error",
            hdrs=None,
            fp=io.BytesIO(b'{"error":"CUDA out of memory while allocating KV cache"}'),
        )
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            failure,
        ]), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("HTTP 500 Internal Server Error", errors.getvalue())
        self.assertIn("CUDA out of memory while allocating KV cache", errors.getvalue())

    def test_http_500_body_is_preserved_for_vision_generation_without_blocking_chat(self) -> None:
        errors = io.StringIO()
        failure = urllib.error.HTTPError(
            "http://127.0.0.1:11434/api/chat",
            500,
            "Internal Server Error",
            hdrs=None,
            fp=io.BytesIO(b'{"error":"runner process has terminated"}'),
        )
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "qwen3.5:9b"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            failure,
            {"models": [{"name": "qwen3.5:9b", "size_vram": 123456}]},
        ]), contextlib.redirect_stderr(errors):
            self.assertTrue(probe.preflight(timeout=1))
        self.assertIn("image-bearing chat failed", errors.getvalue())
        self.assertIn("runner process has terminated", errors.getvalue())

    def test_api_failure_is_reported_without_network_access(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=OSError("simulated offline API")):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertIn("simulated offline API", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
