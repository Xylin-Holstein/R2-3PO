#!/usr/bin/env python3
"""Guard the bounded RAM context cache and per-inference hardware telemetry."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


for token, message in (
    ("#define R2_CONTEXT_CACHE_ENTRIES 12", "RAM cache has a fixed entry bound"),
    ("#define R2_CONTEXT_CACHE_TTL_SECONDS 12", "RAM cache entries expire quickly"),
    ("#define R2_CONTEXT_CACHE_MAX_VALUE (32U * 1024U)", "each cached value has a hard size limit"),
    ("clock_gettime(CLOCK_MONOTONIC", "cache expiry uses monotonic time rather than the private wall clock"),
    ("context_cache_invalidate();", "writes invalidate cached evidence"),
    ("context_cache_store(query ? query : \"\", MAX_RELEVANT_MEMORIES,", "persistent-memory query results are cached in RAM"),
    ("context_cache_get(query ? query : \"\",", "the conversation path reuses cached persistent-memory results"),
    (core.find("context_cache_store(query ? query : \\"\\", MAX_RELEVANT_MEMORIES,") < core.find("Conversation continuity: the Life Log"), "Life Log history is searched fresh after the cached memory lookup"),
    ("char *visual_context = NULL;", "live visual context is still assembled separately on each turn"),
    ("char *context = r2_reality_context();", "Reality remains refreshed rather than served from historical cache"),
    ("prompt_tokens=%lld prompt_s=%.3f", "inference logs include prompt processing metrics"),
    ("generated_tokens=%lld generation_s=%.3f tok_s=%.2f", "inference logs include generated tokens and measured generation rate"),
    ("gpu_layer_policy=%s available_ram_gib=%.2f", "inference logs expose GPU policy and current available RAM"),
):
    require(token in core, message)

print("RAM context cache and hardware telemetry smoke passed.")
