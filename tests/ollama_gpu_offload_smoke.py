#!/usr/bin/env python3
"""Check NVIDIA partial-offload configuration and runtime placement diagnostics."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
service = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
runner = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
readme = (ROOT / "README.md").read_text(encoding="utf-8")
preflight = (ROOT / "tools/preflight_ollama_model.py").read_text(encoding="utf-8")

required = (
    'getenv("R2_OLLAMA_NUM_GPU")',
    'R2_OLLAMA_ADD_OPTION("num_gpu"',
    "gpu_layers >= -1 && gpu_layers <= 999",
    "if (gpu_layers != 0)",
)
for needle in required:
    if needle not in core:
        raise SystemExit(f"Missing configurable GPU layer behavior: {needle}")

if "GTX 1050 Ti" not in launcher or "export R2_OLLAMA_NUM_GPU=20" not in launcher:
    raise SystemExit("Interactive launcher must request partial offload when the GTX 1050 Ti is detected.")
if 'R2_OLLAMA_NUM_GPU="${R2_OLLAMA_NUM_GPU}"' not in launcher:
    raise SystemExit("Interactive launcher must pass GPU layer policy through the sanitized runtime environment.")
if 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' not in launcher:
    raise SystemExit("Interactive launcher must preserve the optional adaptive context override.")
if 'getenv("R2_OLLAMA_NUM_GPU")' not in visual or 'json_object_object_add(options, "num_gpu"' not in visual:
    raise SystemExit("Vision requests must use the same GPU layer policy as chat.")
if "GTX 1050 Ti" not in runner or "export R2_OLLAMA_NUM_GPU=20" not in runner:
    raise SystemExit("Overnight service runner must select partial offload when the GTX 1050 Ti is present.")
if '"R2_OLLAMA_NUM_GPU=$R2_OLLAMA_NUM_GPU"' not in runner:
    raise SystemExit("Overnight service runner must pass the GPU policy to the R2 process.")
if 'options["num_gpu"] = gpu_layers' not in preflight or 'f"{api_base}/api/ps"' not in preflight:
    raise SystemExit("Ollama preflight must send the configured layer policy and inspect actual VRAM placement.")
if "size_vram" not in preflight or "nvidia-smi" not in preflight:
    raise SystemExit("Ollama preflight must report model VRAM and visible NVIDIA hardware.")
if "R2_OLLAMA_NUM_GPU=20" not in readme or "4 GiB VRAM" not in readme:
    raise SystemExit("Documentation must explain the GTX 1050 Ti partial-offload policy.")

print("Ollama NVIDIA GPU offload configuration checks passed.")
