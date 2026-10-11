#!/usr/bin/env python3
"""Check safe automatic placement by default and preserve explicit GPU overrides."""
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

if 'R2_OLLAMA_NUM_GPU:-0' not in launcher:
    raise SystemExit("Interactive launcher must default to Ollama automatic placement.")

if 'R2_OLLAMA_NUM_GPU="${R2_OLLAMA_NUM_GPU}"' not in launcher:
    raise SystemExit("Interactive launcher must pass the GPU offload setting through sudo's sanitized environment.")
if 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' not in launcher:
    raise SystemExit("Interactive launcher must preserve the optional adaptive context override.")
if 'getenv("R2_OLLAMA_NUM_GPU")' not in visual or 'json_object_object_add(options, "num_gpu"' not in visual:
    raise SystemExit("Vision requests must use the same GPU layer policy as chat.")
if "Environment=R2_OLLAMA_NUM_GPU=0" not in service:
    raise SystemExit("Overnight service must default to Ollama automatic placement.")
if "leaving layer placement to Ollama" not in readme or "100% CPU" not in readme:
    raise SystemExit("Documentation must explain automatic placement and CPU-only diagnosis.")

print("Ollama GPU offload configuration checks passed.")
