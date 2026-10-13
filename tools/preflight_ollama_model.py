#!/usr/bin/env python3
"""Verify the configured local multimodal model can complete a real chat request before R2 starts."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any

API_BASE = os.environ.get("R2_OLLAMA_BASE_URL", "http://127.0.0.1:11434").rstrip("/")
MODEL = os.environ.get("R2_MODEL", "gemma3:4b").strip() or "gemma3:4b"


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

    gpu_layers_raw = os.environ.get("R2_OLLAMA_NUM_GPU", "0").strip() or "0"
    try:
        gpu_layers = int(gpu_layers_raw)
    except ValueError:
        print(f"ERROR: Invalid R2_OLLAMA_NUM_GPU={gpu_layers_raw!r}; use -1, 0, or a positive layer count.", file=sys.stderr)
        return False
    if gpu_layers < -1 or gpu_layers > 999:
        print("ERROR: R2_OLLAMA_NUM_GPU must be between -1 and 999.", file=sys.stderr)
        return False

    options: dict[str, Any] = {"temperature": 0, "num_predict": 128}
    if gpu_layers != 0:
        options["num_gpu"] = gpu_layers

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
                "think": False,
                "keep_alive": "10m",
                "options": options,
            },
        )
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(f"ERROR: Ollama listed {MODEL} but real chat generation failed: {exc}", file=sys.stderr)
        return False

    message = reply.get("message")
    content = message.get("content") if isinstance(message, dict) else None
    if not isinstance(content, str) or not content.strip():
        thinking = message.get("thinking") if isinstance(message, dict) else None
        if isinstance(thinking, str) and thinking.strip():
            detail = "Ollama returned thinking output but no final answer text, even with think=false."
        else:
            detail = "Ollama returned an empty final answer field."
        print(f"ERROR: Ollama listed {MODEL} but chat generation produced no usable final answer. {detail}", file=sys.stderr)
        return False

    print(f"Local Ollama is ready: {MODEL} completed a real chat generation (reply omitted).")
    if gpu_layers == 0:
        print("GPU policy: automatic placement (no explicit layer count sent).")
    else:
        print(f"GPU policy: requested {gpu_layers} model layers on the GPU.")

    # Keep the model warm during this check, then inspect Ollama's own process
    # report. A successful API request alone does not prove GPU offload.
    try:
        ps = _read_json(f"{API_BASE}/api/ps", timeout=min(8, max(1, timeout)))
        loaded = ps.get("models", [])
        active = next((item for item in loaded
                       if isinstance(item, dict)
                       and (item.get("name") == MODEL
                            or str(item.get("name", "")).startswith(MODEL + "-"))), None)
        if active is None:
            print("GPU verification: model not present in /api/ps; placement remains unverified.")
        else:
            vram = active.get("size_vram", 0)
            try:
                vram = int(vram)
            except (TypeError, ValueError):
                vram = 0
            if vram > 0:
                print(f"GPU verification: Ollama reports {vram / (1024 ** 3):.2f} GiB allocated in GPU VRAM.")
            else:
                print("GPU verification: Ollama reports no VRAM allocation; inference may be CPU-only.")
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(f"GPU verification unavailable: could not query Ollama /api/ps ({exc}).")

    # Host-side diagnostic; lack of nvidia-smi access does not fail preflight.
    try:
        gpu = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total,memory.used", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=5, check=False,
        )
        if gpu.returncode == 0 and gpu.stdout.strip():
            print("NVIDIA hardware: " + "; ".join(gpu.stdout.strip().splitlines()))
        else:
            print("NVIDIA hardware: nvidia-smi did not report a usable device; check Ollama logs.")
    except (OSError, subprocess.TimeoutExpired):
        print("NVIDIA hardware: nvidia-smi unavailable; GPU backend detection is not confirmed.")
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
