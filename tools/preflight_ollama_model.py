#!/usr/bin/env python3
"""Verify the local Qwen 3.5 multimodal model can perform a real chat completion before R2 starts."""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import struct
import zlib
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any

DEFAULT_API_BASE = "http://127.0.0.1:11434"
ALLOWED_API_BASES = {
    "http://127.0.0.1:11434",
    "http://127.0.0.1:11435",
}
MODEL = "qwen3.5:9b"


def _configured_api_base() -> str:
    """Return only an explicitly approved loopback Ollama endpoint."""
    base = os.environ.get("R2_OLLAMA_BASE_URL", DEFAULT_API_BASE)
    if base not in ALLOWED_API_BASES:
        raise ValueError(
            "R2_OLLAMA_BASE_URL must be http://127.0.0.1:11434 or "
            "http://127.0.0.1:11435"
        )
    return base


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


def _http_error_detail(exc: urllib.error.HTTPError) -> str:
    """Preserve Ollama's useful JSON error instead of reporting only HTTP 500."""
    try:
        raw = exc.read(4096).decode("utf-8", errors="replace").strip()
    except (OSError, ValueError):
        raw = ""
    if raw:
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            detail = raw
        else:
            detail = payload.get("error", raw) if isinstance(payload, dict) else raw
        # Keep terminal output useful without dumping an arbitrarily large server body.
        detail = str(detail).replace("\r", " ").replace("\n", " ").strip()
        if len(detail) > 1200:
            detail = detail[:1197] + "..."
        return f"HTTP {exc.code} {exc.reason}: {detail}"
    return f"HTTP {exc.code} {exc.reason} (Ollama returned no error body)"


def _is_complete_assistant_turn(reply: dict[str, Any]) -> bool:
    """Match the core's non-streaming Ollama completion contract."""
    message = reply.get("message")
    return (
        isinstance(message, dict)
        and message.get("role") == "assistant"
        and reply.get("done") is True
    )


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def _vision_probe_image_base64() -> str:
    """Build a camera-sized 640x480 known-color PNG using only the standard library."""
    width, height = 640, 480
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)  # PNG filter: None
        for x in range(width):
            if 170 <= x < 470 and 90 <= y < 390:
                scanlines.extend((0, 0, 255, 255))  # solid blue square
            else:
                scanlines.extend((255, 255, 255, 255))  # white background
    png = (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + _png_chunk(b"IDAT", zlib.compress(bytes(scanlines), level=6))
        + _png_chunk(b"IEND", b"")
    )
    return base64.b64encode(png).decode("ascii")


def preflight(wait_seconds: float = 0, timeout: float = 180, require_gpu_offload: bool = False) -> bool:
    """Check local model availability and one real, bounded inference."""
    try:
        api_base = _configured_api_base()
    except ValueError as exc:
        print(f"ERROR: Local Ollama/model preflight failed: {exc}", file=sys.stderr)
        return False

    deadline = time.monotonic() + max(0, wait_seconds)
    last_error: Exception | None = None
    while True:
        try:
            tags = _read_json(f"{api_base}/api/tags", timeout=min(4, max(1, timeout)))
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

    # Exercise the same reasoning-mode sampler as foreground conversation.
    # A non-thinking token probe can pass while R2's actual think=true request
    # still fails under the same context/GPU allocation.
    options: dict[str, Any] = {
        "temperature": 1.0,
        "top_p": 0.95,
        "top_k": 20,
        "min_p": 0.0,
        "presence_penalty": 1.5,
        "repeat_penalty": 1.0,
        "num_predict": 512,
    }
    configured_context = os.environ.get("R2_OLLAMA_NUM_CTX", "").strip()
    if configured_context:
        try:
            context_tokens = int(configured_context, 10)
        except ValueError:
            context_tokens = 0
        # Mirror the core's accepted override range. Invalid values are ignored
        # by both paths so preflight does not test a context R2 will not use.
        if 2048 <= context_tokens <= 16384:
            options["num_ctx"] = context_tokens
    if gpu_layers != 0:
        options["num_gpu"] = gpu_layers

    try:
        reply = _read_json(
            f"{api_base}/api/chat",
            timeout=timeout,
            payload={
                "model": MODEL,
                "messages": [{
                    "role": "user",
                    "content": "Reply with exactly this token and no other text: R2_READY",
                }],
                "stream": False,
                "think": True,
                "keep_alive": "10m",
                "options": options,
            },
        )
    except urllib.error.HTTPError as exc:
        print(
            f"ERROR: Ollama listed {MODEL} but real chat generation failed: {_http_error_detail(exc)}",
            file=sys.stderr,
        )
        return False
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


    if not _is_complete_assistant_turn(reply):
        print(
            f"ERROR: Ollama listed {MODEL} but text generation did not return a complete assistant turn "
            "(expected message.role=assistant and done=true).",
            file=sys.stderr,
        )
        return False

    # Exercise the same 640x480 image dimensions, JSON response mode, GPU/context
    # policy and context-dependent output cap used by Visual.c. This catches
    # image-encoder/KV-cache OOMs that text-only readiness can miss.
    vision_context = int(options.get("num_ctx", 4096))
    vision_options = dict(options)
    vision_options["num_predict"] = 512 if vision_context >= 8192 else 256
    # Vision is exercised here to report capability, but typed conversation is
    # the launch-critical path. A vision OOM or bad image answer must not prevent
    # R2 from starting its text conversation; keep the failure visible for later
    # visual-subsystem debugging.
    vision_ready = True
    try:
        vision_reply = _read_json(
            f"{api_base}/api/chat",
            timeout=timeout,
            payload={
                "model": MODEL,
                "format": "json",
                "messages": [
                    {
                        "role": "system",
                        "content": "Return only a JSON object with a string field named color. Identify the visible color of the large square.",
                    },
                    {
                        "role": "user",
                        "content": "What color is the large square? Return JSON with the key color.",
                        "images": [_vision_probe_image_base64()],
                    },
                ],
                "stream": False,
                "think": False,
                "keep_alive": "10m",
                "options": vision_options,
            },
        )
    except urllib.error.HTTPError as exc:
        print(
            f"WARNING: {MODEL} passed text generation but image-bearing chat failed: {_http_error_detail(exc)}",
            file=sys.stderr,
        )
        vision_ready = False
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(
            f"WARNING: {MODEL} passed text generation but image-bearing chat failed: {exc}",
            file=sys.stderr,
        )
        vision_ready = False

    if vision_ready and not _is_complete_assistant_turn(vision_reply):
        print(
            f"WARNING: {MODEL} returned an incomplete or invalid assistant turn for the image-bearing probe "
            "(expected message.role=assistant and done=true); visual inference is not verified.",
            file=sys.stderr,
        )
        vision_ready = False

    if vision_ready:
        vision_message = vision_reply.get("message")
        vision_content = vision_message.get("content") if isinstance(vision_message, dict) else None
        try:
            vision_data = json.loads(vision_content) if isinstance(vision_content, str) else {}
        except json.JSONDecodeError:
            vision_data = {}
        vision_color = vision_data.get("color") if isinstance(vision_data, dict) else None
        if not isinstance(vision_color, str) or not re.search(r"\bblue\b", vision_color, re.IGNORECASE):
            print(
                f"WARNING: {MODEL} passed text generation but failed the JSON known-color vision probe "
                "(expected the image-bearing reply to report color=blue); visual inference is not verified.",
                file=sys.stderr,
            )
            vision_ready = False

    gpu_offload_verified = False
    mixed_placement_verified = False
    if gpu_layers == 0:
        print("GPU policy: automatic placement (no explicit layer count sent).")
    else:
        print(f"GPU policy: requested {gpu_layers} model layers on the GPU.")

    # Keep the model warm during this check, then inspect Ollama's own process
    # report. A successful API request alone does not prove GPU offload.
    try:
        ps = _read_json(f"{api_base}/api/ps", timeout=min(8, max(1, timeout)))
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
                gpu_offload_verified = True
                print(f"GPU verification: Ollama reports {vram / (1024 ** 3):.2f} GiB allocated in GPU VRAM.")
                total_size = active.get("size", 0)
                try:
                    total_size = int(total_size)
                except (TypeError, ValueError):
                    total_size = 0
                if total_size > vram:
                    mixed_placement_verified = True
                    print(f"Mixed placement: model process reports {total_size / (1024 ** 3):.2f} GiB total with {vram / (1024 ** 3):.2f} GiB in VRAM; remaining model/runtime memory is outside GPU VRAM.")
            else:
                print("GPU verification: Ollama reports no VRAM allocation; inference may be CPU-only.")
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(f"GPU verification unavailable: could not query Ollama /api/ps ({exc}).")

    # When the host explicitly requests GPU layers, a successful text reply is
    # not enough: require Ollama's process report to confirm that some model
    # memory really landed in VRAM. CPU-only success must not masquerade as a
    # successful GTX 1050 Ti policy preflight.
    if require_gpu_offload and (
        not gpu_offload_verified or not mixed_placement_verified
    ):
        print(
            "ERROR: mixed CPU/GPU placement was required but Ollama did not report "
            "both positive VRAM allocation and total model memory greater than VRAM.",
            file=sys.stderr,
        )
        return False

    if vision_ready:
        print(f"Local Ollama is ready: {MODEL} passed real text and image-bearing chat generations (replies omitted).")
    else:
        print(f"Local Ollama text chat is ready: {MODEL} passed real text generation; vision probe failed and visual inference is not verified.")

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
    parser.add_argument("--require-gpu-offload", action="store_true",
                        help="fail unless Ollama reports both VRAM allocation and model memory outside VRAM")
    args = parser.parse_args()
    if args.timeout <= 0 or args.wait_seconds < 0:
        parser.error("--timeout must be positive and --wait-seconds cannot be negative")
    return 0 if preflight(args.wait_seconds, args.timeout, args.require_gpu_offload) else 1


if __name__ == "__main__":
    raise SystemExit(main())
