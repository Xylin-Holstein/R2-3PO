#!/usr/bin/env python3
"""Keep the full R2 instructions in the reduced-GPU context fallback."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
runner = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

for label, source in (("interactive launcher", launcher), ("overnight service", runner)):
    assert "nvidia-smi --query-gpu=name --format=csv,noheader" in source, label
    assert "grep -qi 'GTX 1050 Ti'" in source, label
    if label == "interactive launcher":
        assert "R2 will select context from available RAM headroom" in source, label
    else:
        assert "memory-aware context allocation" in source, label
    assert "export R2_OLLAMA_NUM_GPU=8" in source, label
    assert '[[ -z "${R2_OLLAMA_NUM_CTX:-}" && "$R2_NVIDIA_QUERY_OK" == "1" ]]' in source, label
assert '"R2_OLLAMA_NUM_CTX=${R2_OLLAMA_NUM_CTX:-}"' in runner
for label, source in (("interactive launcher", launcher), ("overnight service", runner)):
    assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in source, label
    assert '"$R2_OLLAMA_NUM_CTX_DEFAULTED" == "1" ]]; then' in source, label
    assert "export R2_OLLAMA_NUM_GPU=4" in source, label
    assert "export R2_OLLAMA_NUM_CTX=12288" in source, label
assert 'R2_OLLAMA_NUM_CTX_DEFAULTED="${R2_OLLAMA_NUM_CTX_DEFAULTED:-0}"' in runner
assert 'R2_OLLAMA_NUM_GPU_DEFAULTED="${R2_OLLAMA_NUM_GPU_DEFAULTED:-0}"' in runner
assert "export R2_OLLAMA_NUM_CTX_DEFAULTED" in runner
assert "export R2_OLLAMA_NUM_GPU_DEFAULTED" in runner
assert "R2_OLLAMA_NUM_CTX_DEFAULTED=0" in launcher
assert "R2_OLLAMA_NUM_GPU_DEFAULTED=0" in launcher
assert 'if [[ -z "${R2_OLLAMA_NUM_GPU:-}" ]]; then' in launcher
assert 'if [[ -z "${R2_OLLAMA_NUM_GPU:-}" ]]; then' in runner
assert 'if [[ -z "${R2_OLLAMA_NUM_GPU+x}" ]]; then' not in launcher
assert 'if [[ -z "${R2_OLLAMA_NUM_GPU+x}" ]]; then' not in runner
assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in runner
assert '"$R2_OLLAMA_NUM_CTX_DEFAULTED" == "1" ]]; then' in runner
assert "export R2_OLLAMA_NUM_GPU=4" in runner
assert "export R2_OLLAMA_NUM_CTX=12288" in runner
assert 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' in launcher
assert 'requested >= 2048 && requested <= 16384' in core
assert 'if (requested < safe_max) safe_max = (int)requested;' in core
assert "r2_context_for_required_chars(required_chars, num_ctx)" in core
assert "to fit its complete instructions and active turn." in core
assert "msgs[i].pinned && priority <= 3" in core
assert "refusing inference: a complete mandatory static instruction " in core
assert "section does not fit the allocated context" in core
assert "required_static_chars = sum(len(message[\"content\"]) for message in static_messages) + 1500" in (ROOT / "tools/preflight_ollama_model.py").read_text(encoding="utf-8")
assert "if (num_predict < 1) num_predict = max_predict;" in core, "default output allocation must use the full context-derived ceiling"
assert "int max_predict = num_ctx >= 16384 ? 4096 :" in core
assert "num_ctx >= 12288 ? 2048 :" in core
assert "num_ctx >= 8192 ? 1024 :" in core
assert "num_ctx >= 4096 ? 512 : 256;" in core

print("GTX 1050 Ti context defaults and explicit override path checks passed.")
