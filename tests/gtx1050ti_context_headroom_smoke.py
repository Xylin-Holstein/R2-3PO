#!/usr/bin/env python3
"""Keep the 4 GiB GTX 1050 Ti launch default from overcommitting KV-cache VRAM."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
start = launcher.find("R2_HAS_GTX_1050_TI=0")
end = launcher.find("# The interactive launcher and the overnight service share persistent databases.", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate the GPU/context policy in the launcher.")
block = launcher[start:end]

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require("nvidia-smi --query-gpu=name --format=csv,noheader" in block
        and "grep -qi 'GTX 1050 Ti'" in block,
        "the context cap is gated on detecting the host's 4 GiB GTX 1050 Ti")
require('if [[ "$R2_HAS_GTX_1050_TI" == "1" && -z "${R2_OLLAMA_NUM_CTX:-}" ]]' in block
        and "export R2_OLLAMA_NUM_CTX=8192" in block,
        "the default context is capped at 8192 only when the operator did not configure one")
require("R2_OLLAMA_NUM_CTX to override" in block
        and 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' in launcher,
        "an explicit context setting remains available and is passed into the R2 process")
print("GTX 1050 Ti context headroom regression passed.")
