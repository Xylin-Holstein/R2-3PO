#!/usr/bin/env python3
"""Verify history is bounded by the real inference window, not fixed message quotas."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


require("for (size_t i = count; i > 0; --i)" in core,
        "inference history selection walks all available messages newest-first")
require("OLLAMA_MAX_RECENT_MESSAGES" not in core,
        "there is no fixed 16-message history cutoff")
require("OLLAMA_MAX_MESSAGE_CHARS" not in core and
        "OLLAMA_MAX_SYSTEM_MESSAGE_CHARS" not in core,
        "there are no arbitrary per-message prompt caps")
require("size_t input_budget = (size_t)(num_ctx - num_predict - 512) * 32U / 10U;" in core,
        "the actual Ollama context window determines the payload budget")
serializer_start = core.find("static char *ollama_chat_with_limit(")
serializer_end = core.find("\\nstatic char *ollama_chat(", serializer_start)
serializer = core[serializer_start:serializer_end]
require(serializer_start >= 0 and serializer_end > serializer_start,
        "the real Ollama serializer can be inspected")
require("if (length > remaining) continue;" in serializer and
        "r2_utf8_safe_prefix_length(content, remaining)" not in serializer,
        "only complete messages are selected against the real remaining context budget")

print("Recent conversation context budget smoke passed.")
