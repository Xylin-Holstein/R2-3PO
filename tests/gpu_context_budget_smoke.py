#!/usr/bin/env python3
"""Keep the target 4 GiB GPU's default context within a safer KV-cache budget."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
runner = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
bootstrap = (ROOT / "tools/r2_install_root_bootstrap.sh").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

for label, source in (("interactive launcher", launcher), ("overnight service", runner)):
    assert "nvidia-smi --query-gpu=name --format=csv,noheader" in source, label
    assert "grep -qi 'GTX 1050 Ti'" in source, label
    assert '[[ -z "${R2_OLLAMA_NUM_CTX:-}" && "$R2_NVIDIA_QUERY_OK" == "1" ]]' in source, label
    assert "export R2_OLLAMA_NUM_CTX=4096" in source, label
    assert 'if [[ -z "${R2_OLLAMA_NUM_GPU+x}" ]]; then' in source, label
    assert "export R2_OLLAMA_NUM_GPU=12" in source, label
    assert "R2_OLLAMA_NUM_CTX_DEFAULTED" not in source, label
    assert "R2_OLLAMA_NUM_GPU_DEFAULTED" not in source, label
    assert "export R2_OLLAMA_NUM_CTX=8192" not in source, label
    assert "export R2_OLLAMA_NUM_GPU=20" not in source, label
    assert "retrying with 12 GPU layers and 4096 context tokens" not in source, label

assert 'R2_OLLAMA_NUM_GPU="${R2_OLLAMA_NUM_GPU}"' in launcher
assert 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' in launcher
assert '"R2_OLLAMA_NUM_CTX=${R2_OLLAMA_NUM_CTX:-}"' in runner
assert '"R2_OLLAMA_NUM_GPU=$R2_OLLAMA_NUM_GPU"' in runner
assert 'requested >= 2048 && requested <= 16384' in core
assert 'if (requested < safe_max) safe_max = (int)requested;' in core
assert 'PREFLIGHT_GPU=12' in bootstrap
assert 'PREFLIGHT_CTX=4096' in bootstrap
assert 'PREFLIGHT_GPU=20' not in bootstrap
assert 'PREFLIGHT_CTX=8192' not in bootstrap
assert 'PREFLIGHT_POLICY_DEFAULTED' not in bootstrap
assert 'retrying with 12 GPU layers and 4096 context tokens' not in bootstrap
print("GTX 1050 Ti conservative context defaults and explicit override checks passed.")
