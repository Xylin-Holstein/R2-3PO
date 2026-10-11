#!/usr/bin/env python3
"""Unit smoke tests for the local Ollama readiness probe; never opens a socket."""
from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import preflight_ollama_model as probe  # noqa: E402


class OllamaPreflightSmoke(unittest.TestCase):
    def test_model_and_real_chat_completion_are_required(self) -> None:
        replies = [
            {"models": [{"name": "gemma3:4b"}]},
            {"message": {"content": "R2_READY"}},
        ]
        with patch.object(probe, "_read_json", side_effect=replies) as read:
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 2)
        self.assertTrue(read.call_args_list[0].args[0].endswith("/api/tags"))
        self.assertTrue(read.call_args_list[1].args[0].endswith("/api/chat"))
        self.assertEqual(read.call_args_list[1].kwargs["payload"]["model"], "gemma3:4b")

    def test_missing_model_fails_without_waiting_or_network(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", return_value={"models": []}) as read:
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertEqual(read.call_count, 1)
        self.assertIn("required local model gemma3:4b is not installed", errors.getvalue())

    def test_listed_model_without_chat_text_fails(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "gemma3:4b"}]},
            {"message": {"content": "   "}},
        ]):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("returned no usable chat text", errors.getvalue())

    def test_api_failure_is_reported_without_network_access(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=OSError("simulated offline API")):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertIn("simulated offline API", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
