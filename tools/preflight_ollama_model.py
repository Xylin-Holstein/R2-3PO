#!/usr/bin/env python3
"""Verify the local Gemma model can perform a real chat completion before R2 starts."""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from typing import Any

API_BASE = "http://127.0.0.1:11434"
MODEL = "gemma3:4b"


def _read_json(url: str, timeout: float, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        result = json.load(response)
    if not isinstance(result, dict):
        raise ValueError("Ollama returned a non-object JSON response")
    return result


def preflight(wait_seconds: float = 0, timeout: float = 180) -> bool:
    """Check local model availability and one real, bounded inference."""
    deadline = time.monotonic() + max(0, wait_seconds)
    last_error: Exception | None = None
    while True:
        try:
            tags = _read_json(f"{API_BASE}/api/tags", timeout=min(4, max(1, timeout)))
            models = tags.get("models", [])
            if any(
                isinstance(item, dict)
                and (item.get("name") == MODEL or str(item.get("name", "")).startswith(MODEL + "-"))
                for item in models
            ):
                break
            last_error = RuntimeError(f"required local model {MODEL} is not installed")
        except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
            last_error = exc

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            print(f"ERROR: Local Ollama/model preflight failed: {last_error}", file=sys.stderr)
            return False
        time.sleep(min(1, remaining))

    try:
        reply = _read_json(
            f"{API_BASE}/api/chat",
            timeout=timeout,
            payload={
                "model": MODEL,
                "messages": [{
                    "role": "user",
                    "content": "Reply with exactly this token and no other text: R2_READY",
                }],
                "stream": False,
                "keep_alive": "10m",
                "options": {"temperature": 0, "num_predict": 8},
            },
        )
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(f"ERROR: Ollama listed {MODEL} but real chat generation failed: {exc}", file=sys.stderr)
        return False

    message = reply.get("message")
    content = message.get("content") if isinstance(message, dict) else None
    if not isinstance(content, str) or not content.strip():
        print(f"ERROR: Ollama listed {MODEL} but returned no usable chat text.", file=sys.stderr)
        return False

    print(f"Local Ollama is ready: {MODEL} completed a real chat generation (reply omitted).")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wait-seconds", type=float, default=0,
                        help="wait this long for the local API/model to become available")
    parser.add_argument("--timeout", type=float, default=180,
                        help="maximum seconds for the single real inference request")
    args = parser.parse_args()
    if args.timeout <= 0 or args.wait_seconds < 0:
        parser.error("--timeout must be positive and --wait-seconds cannot be negative")
    return 0 if preflight(args.wait_seconds, args.timeout) else 1


if __name__ == "__main__":
    raise SystemExit(main())
