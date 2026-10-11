#!/usr/bin/env python3
"""Check that R2 requests max-fit GPU offload without removing CPU fallback."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
service = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
readme = (ROOT / "README.md").read_text(encoding="utf-8")

required = (
    'getenv("R2_OLLAMA_NUM_GPU")',
    'json_object_object_add(options, "num_gpu"',
    "gpu_layers >= -1 && gpu_layers <= 999",
    "if (gpu_layers != 0)",
)
for needle in required:
    if needle not in core:
        raise SystemExit(f"Missing configurable GPU layer behavior: {needle}")

if 'R2_OLLAMA_NUM_GPU:-1' not in launcher:
    raise SystemExit("Interactive launcher must request max-fit GPU offload by default.")
if 'getenv("R2_OLLAMA_NUM_GPU")' not in visual or 'json_object_object_add(options, "num_gpu"' not in visual:
    raise SystemExit("Vision requests must use the same GPU layer policy as chat.")
if "Environment=R2_OLLAMA_NUM_GPU=-1" not in service:
    raise SystemExit("Overnight service must use the same GPU offload policy.")
if "leave remaining layers on CPU" not in readme or "100% CPU" not in readme:
    raise SystemExit("Documentation must explain mixed placement and CPU-only diagnosis.")

print("Ollama GPU offload configuration checks passed.")
