#!/usr/bin/env python3
"""Verify the local Qwen 3.5 multimodal model can perform a real chat completion before R2 starts."""
from __future__ import annotations

import argparse
import ast
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
from pathlib import Path

DEFAULT_API_BASE = "http://127.0.0.1:11434"
ALLOWED_API_BASES = {
    "http://127.0.0.1:11434",
    "http://127.0.0.1:11435",
}
MODEL = "qwen3.5:9b"


def _decode_c_string_block(source: str, start_marker: str, end_marker: str) -> str:
    start = source.find(start_marker)
    end = source.find(end_marker, start)
    if start < 0 or end < 0:
        raise ValueError(f"Could not locate full prompt block: {start_marker}")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', source[start:end])
    if not literals:
        raise ValueError(f"Prompt block has no C string literals: {start_marker}")
    try:
        return "".join(ast.literal_eval(literal) for literal in literals)
    except (SyntaxError, ValueError) as exc:
        raise ValueError(f"Could not decode full prompt block: {start_marker}") from exc


def _full_static_instruction_messages() -> list[dict[str, str]]:
    """Load the exact production system prompts used by the foreground request."""
    source_path = Path(__file__).resolve().parents[1] / "r2.c"
    source = source_path.read_text(encoding="utf-8")
    paths_header = (source_path.parent / "r2_diary.h").read_text(encoding="utf-8")
    root_match = re.search(
        r'^#define\s+R2_ROOT\s+("(?:\\.|[^"\\])*")',
        paths_header,
        re.MULTILINE,
    )
    if not root_match:
        raise ValueError("Could not resolve R2_ROOT used inside the production system prompt")
    root_literal = root_match.group(1)
    source = source.replace(" R2_ROOT ", f" {root_literal} ")
    system_prompt = _decode_c_string_block(
        source, "static const char *SYSTEM_PROMPT =", ";\n"
    )
    conversation_prompt = _decode_c_string_block(
        source,
        "static const char r2_conversation_system_prompt[] =",
        "\n\nstatic char *r2_build_retry_system_prompt",
    )
    return [
        {"role": "system", "content": conversation_prompt},
        {"role": "system", "content": system_prompt},
    ]


def _read_uint_file(path: str) -> tuple[int, bool]:
    """Read a cgroup memory counter, preserving valid zero/unlimited values."""
    try:
        with open(path, "r", encoding="ascii") as source:
            raw = source.readline().strip()
    except OSError:
        return 0, False
    if raw == "max":
        return 0, True
    try:
        return int(raw, 10), True
    except ValueError:
        return 0, False


def _available_memory_bytes() -> int:
    """Mirror the core's minimum of host MemAvailable and cgroup headroom."""
    host_available = 0
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as meminfo:
            for line in meminfo:
                if line.startswith("MemAvailable:"):
                    host_available = int(line.split()[1]) * 1024
                    break
    except (OSError, ValueError, IndexError):
        pass

    cgroup_max, max_valid = _read_uint_file("/sys/fs/cgroup/memory.max")
    cgroup_used, used_valid = _read_uint_file("/sys/fs/cgroup/memory.current")
    if max_valid and cgroup_max > 0:
        cgroup_available = (
            1 if not used_valid or cgroup_used >= cgroup_max
            else cgroup_max - cgroup_used
        )
        if not host_available or cgroup_available < host_available:
            host_available = cgroup_available

    if not max_valid:
        v1_max, v1_max_valid = _read_uint_file(
            "/sys/fs/cgroup/memory/memory.limit_in_bytes"
        )
        v1_used, v1_used_valid = _read_uint_file(
            "/sys/fs/cgroup/memory/memory.usage_in_bytes"
        )
        if v1_max_valid and 0 < v1_max < (1 << 60):
            v1_available = (
                1 if not v1_used_valid or v1_used >= v1_max
                else v1_max - v1_used
            )
            if not host_available or v1_available < host_available:
                host_available = v1_available
    return host_available


def _adaptive_context_tokens() -> int:
    """Choose the RAM-based tier before full-prompt requirements are applied."""
    available_bytes = _available_memory_bytes()
    gib = 1024 ** 3
    if not available_bytes:
        return 8192
    if available_bytes >= 8 * gib:
        return 16384
    if available_bytes >= 5 * gib:
        return 12288
    if available_bytes >= 3 * gib:
        return 8192
    if available_bytes >= 2 * gib:
        return 4096
    return 2048


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

    configured_context = os.environ.get("R2_OLLAMA_NUM_CTX", "").strip()
    context_tokens = _adaptive_context_tokens()
    valid_configured_context = False
    if configured_context:
        try:
            requested_context = int(configured_context, 10)
        except ValueError:
            requested_context = 0
        if 2048 <= requested_context <= 16384:
            valid_configured_context = True
            context_tokens = min(context_tokens, requested_context)

    try:
        static_messages = _full_static_instruction_messages()
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"ERROR: Could not load R2's full production instructions for preflight: {exc}", file=sys.stderr)
        return False

    required_static_chars = sum(len(message["content"]) for message in static_messages) + 1500
    def output_budget_for(context: int) -> int:
        return (
            4096 if context >= 16384 else
            2048 if context >= 12288 else
            1024 if context >= 8192 else
            512 if context >= 4096 else 256
        )

    def input_budget_for(context: int) -> int:
        output = output_budget_for(context)
        return max(0, context - output - 512) * 32 // 10

    # Context selection must account for the exact decoded production prompts.
    # The configured context is a starting tier, not a cap on mandatory prompt
    # content. Increase it when needed so the complete instructions can fit.
    if required_static_chars > input_budget_for(context_tokens):
        selected_context = None
        for candidate in (4096, 8192, 12288, 16384):
            if candidate < context_tokens:
                continue
            if required_static_chars <= input_budget_for(candidate):
                selected_context = candidate
                break
        context_tokens = selected_context if selected_context is not None else 16384

    num_predict = output_budget_for(context_tokens)
    input_budget_chars = input_budget_for(context_tokens)
    if required_static_chars > input_budget_chars:
        print(
            "ERROR: Even the maximum supported Ollama context cannot fit R2's complete static instructions "
            f"plus active-turn headroom (context={context_tokens}, output={num_predict}, "
            f"required_chars={required_static_chars}, available_chars={input_budget_chars}).",
            file=sys.stderr,
        )
        return False

    options: dict[str, Any] = {
        "temperature": 0,
        "num_predict": num_predict,
        "num_ctx": context_tokens,
    }
    if gpu_layers != 0:
        options["num_gpu"] = gpu_layers

    try:
        reply = _read_json(
            f"{api_base}/api/chat",
            timeout=timeout,
            payload={
                "model": MODEL,
                "messages": static_messages + [{
                    "role": "user",
                    "content": "Reply with exactly this token and no other text: R2_READY",
                }],
                "stream": False,
                "think": False,
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
            f"ERROR: {MODEL} passed text generation but image-bearing chat failed: {_http_error_detail(exc)}",
            file=sys.stderr,
        )
        return False
    except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
        print(f"ERROR: {MODEL} passed text generation but image-bearing chat failed: {exc}", file=sys.stderr)
        return False

    if not _is_complete_assistant_turn(vision_reply):
        print(
            f"ERROR: {MODEL} returned an incomplete or invalid assistant turn for the image-bearing probe "
            "(expected message.role=assistant and done=true).",
            file=sys.stderr,
        )
        return False

    vision_message = vision_reply.get("message")
    vision_content = vision_message.get("content") if isinstance(vision_message, dict) else None
    try:
        vision_data = json.loads(vision_content) if isinstance(vision_content, str) else {}
    except json.JSONDecodeError:
        vision_data = {}
    vision_color = vision_data.get("color") if isinstance(vision_data, dict) else None
    if not isinstance(vision_color, str) or not re.search(r"\bblue\b", vision_color, re.IGNORECASE):
        print(
            f"ERROR: {MODEL} passed text generation but failed the JSON known-color vision probe "
            "(expected the image-bearing reply to report color=blue).",
            file=sys.stderr,
        )
        return False

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

    print(f"Local Ollama is ready: {MODEL} passed real text and image-bearing chat generations (replies omitted).")

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
