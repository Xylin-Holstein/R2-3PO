#!/usr/bin/env python3
"""Keep the interactive launcher, Makefile, model, and cleanup policy in sync."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


sources_match = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.MULTILINE)
headers_match = re.search(r"^HEADERS\s*=\s*(.+)$", makefile, re.MULTILINE)
required_match = re.search(
    r"^REQUIRED_FILES=\(\n(?P<body>.*?)\n\)",
    launcher,
    re.MULTILINE | re.DOTALL,
)
compile_start = launcher.find('sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2')
compile_end = launcher.find("\nstatus=$?", compile_start)
require(sources_match is not None, "Makefile declares its complete production source list")
require(headers_match is not None, "Makefile declares its complete public/internal header list")
require(required_match is not None, "launcher checks its complete required-file inventory")
require(compile_start >= 0 and compile_end > compile_start,
        "launcher has a bounded strict-build command")
if not all((sources_match, headers_match, required_match)) or compile_end <= compile_start:
    raise SystemExit(1)

sources = sources_match.group(1).split()
headers = headers_match.group(1).split()
compile_block = launcher[compile_start:compile_end]
compiled = re.findall(r'"\$R2_SOURCE/([^"]+\.c)"', compile_block)
required = re.findall(r'"\$R2_SOURCE/([^"]+)"', required_match.group("body"))
require(compiled == sources,
        "launcher compiles every Makefile source exactly once and in the same order")
require(len(compiled) == len(set(compiled)),
        "launcher compile list contains no duplicate source files")
require(all(name in required for name in headers),
        "launcher preflight requires every Makefile header before compiling")
require(all(name in required for name in sources),
        "launcher preflight requires every compiled C source before compiling")

model_match = re.search(r'^#define\s+R2_OLLAMA_MODEL\s+"([^"]+)"',
                        header, re.MULTILINE)
require(model_match is not None, "shared model constant exists")
if model_match:
    model = model_match.group(1)
    require(f'R2_VISION_MODEL="{model}"' in launcher,
            "launcher vision model matches the single compiled conversation model")
    require(f"Unified conversation + vision model: {model}" in launcher,
            "launcher status text matches the configured unified model")

require('R2_OLLAMA_TIMEOUT_SECONDS="${R2_OLLAMA_TIMEOUT_SECONDS:-}"' in launcher,
        "launcher passes the configured long-running inference timeout")
preflight_start = launcher.find("r2_run_ollama_preflight() {")
preflight_end = launcher.find("\n}", preflight_start)
preflight_helper = launcher[preflight_start:preflight_end] if preflight_start >= 0 and preflight_end > preflight_start else ""
require(
    '2>&1 | sudo -n -u r2 -- tee -a "$R2_ROOT/R2_Log/Debug.txt"' in preflight_helper
    and 'pipeline_status=("${PIPESTATUS[@]}")' in preflight_helper
    and 'return "${pipeline_status[0]:-1}"' in preflight_helper
    and launcher.count("if ! r2_run_ollama_preflight; then") == 4,
    "all four Ollama preflight attempts stream full diagnostics to Debug.txt while preserving the model-check exit status",
)
require('R2_FX_DIR="$R2_FX_DIR"' in launcher
        and 'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"' in launcher,
        "launcher uses the canonical, overridable sound-effects directory")
require('trap r2_network_cleanup EXIT' in launcher
        and 'r2_network_cleanup() {' in launcher
        and 'r2_observer_cleanup' in launcher,
        "EXIT cleanup always handles observer/display permissions and network rules")
require(re.search(r"\bsudo -u r2\b", launcher) is None,
        "all optional r2 helper/observer launches are non-interactive and cannot hang on a sudo prompt")
cleanup_start = launcher.find("r2_observer_cleanup() {")
cleanup_end = launcher.find("\nr2_remove_network_jumps()", cleanup_start)
cleanup = launcher[cleanup_start:cleanup_end]
require('kill "$OBSERVER_PID"' in cleanup
        and 'wait "$OBSERVER_PID"' in cleanup
        and 'xhost -SI:localuser:r2' in cleanup,
        "observer cleanup terminates/reaps its process and revokes X access")
require('TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"' in launcher
        and 'mv -f "$TEMP_EXEC" "$R2_EXEC"' in launcher,
        "launcher preserves the last known-good executable until a successful build")
print("Launcher/core/module consistency smoke test passed.")
