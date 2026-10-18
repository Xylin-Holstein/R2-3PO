#!/usr/bin/env python3
"""Ensure all documented R2 build paths use the same strict source/config policy."""
from pathlib import Path
import hashlib
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
installer = (ROOT / "tools/r2_install_root_bootstrap.sh").read_text(encoding="utf-8")
overnight_docs = (ROOT / "docs/overnight-service.md").read_text(encoding="utf-8")
installer_wrapper = (ROOT / "tools/install_r2_overnight.sh").read_text(encoding="utf-8")
service_runner = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
hardware_preflight = (ROOT / "tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")

required_sources = (
    "r2.c", "shell.c", "r2_diary.c", "Log.c", "Reality.c", "Addiction.c",
    "Reward.c", "AlternateSelf.c", "Visual.c", "Ears.c", "Eyes.c",
    "R2Sounds.c", "R2Reply.c",
)

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require(
    'run_as_r2 python3 "$MODEL_COPY_SCRIPT"' in installer
    and 'run_as_r2 make -C "$R2_SOURCE" clean all TARGET="$TEMP_EXEC"' in installer
    and 'root_run python3 "$MODEL_COPY_SCRIPT"' not in installer
    and 'root_run make -C "$R2_SOURCE"' not in installer,
    "mutable model-copy script and build system run as r2, never as root",
)

require(
    '[[ "$EUID" -eq 0 ]]' in installer
    and 'PATH="/usr/sbin:/usr/bin:/sbin:/bin"' in installer
    and 'umask 077' in installer
    and 'BOOTSTRAP_PATH="/usr/local/sbin/r2-3po-install"' in installer
    and 'refusing privileged execution outside the root-owned bootstrap path' in installer
    and "R2_OLLAMA_UNIT_EOF" in installer
    and "R2_SERVICE_UNIT_EOF" in installer
    and "--require-gpu-offload" in installer
    and "--require-gpu-offload" in service_runner
    and "--require-gpu-offload" in launcher
    and "for DEVICE_GROUP in audio video; do" in launcher
    and "R2_NVIDIA_PCI_PRESENT=0" in launcher
    and "R2_NVIDIA_PCI_PRESENT=0" in service_runner
    and '[[ "$R2_NVIDIA_VENDOR" == "0x10de" && "$R2_NVIDIA_CLASS" == 0x03* ]]' in launcher
    and '[[ "$R2_NVIDIA_VENDOR" == "0x10de" && "$R2_NVIDIA_CLASS" == 0x03* ]]' in service_runner
    and "NVIDIA_PCI_PRESENT=0" in installer
    and '[[ "$NVIDIA_VENDOR" == "0x10de" && "$NVIDIA_CLASS" == 0x03* ]]' in installer
    and "NVIDIA_PCI_PRESENT=0" in hardware_preflight
    and '[[ "$NVIDIA_VENDOR" == "0x10de" && "$NVIDIA_CLASS" == 0x03* ]]' in hardware_preflight
    and "refusing CPU-only fallback" in launcher
    and "refusing CPU-only fallback" in service_runner
    and "refusing to silently deploy a CPU-only policy" in installer
    and "refusing to treat this as a CPU-only host" in hardware_preflight
    and 'root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-ollama.service"' in installer
    and 'root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-3po.service"' in installer
    and 'root_run install -m 0644 "$OLLAMA_UNIT_SOURCE"' not in installer
    and 'root_run install -m 0644 "$UNIT_SOURCE"' not in installer,
    "privileged bootstrap is root-only and installs embedded trusted unit definitions, not mutable source-tree units",
)
require(
    "do not execute this mutable source-tree script as root" in installer_wrapper
    and "/usr/local/sbin/r2-3po-install" in installer_wrapper
    and "root_run " not in installer_wrapper,
    "source-tree installer does not perform privileged deployment",
)
require(
    'root_run chown r2:r2 "$TEMP_EXEC"' not in installer
    and 'root_run chown r2:r2 "$R2_EXEC"' not in installer
    and 'root_run chown r2:r2 "$BACKUP_EXEC"' not in installer
    and re.search(r'^chmod 0755 "\$RUNNER_SOURCE" "\$OLLAMA_RUNNER_SOURCE"$',
                  installer, re.MULTILINE) is None
    and 'run_as_r2 chmod 0755 "$RUNNER_SOURCE" "$OLLAMA_RUNNER_SOURCE"' in installer
    and 'run_as_r2 mv -f "$TEMP_EXEC" "$R2_EXEC"' in installer,
    "writable runtime paths are not passed to privileged ownership-changing operations",
)

require(
    re.search(r"^CFLAGS\s*\?=\s*-std=c11\s+-Wall\s+-Wextra\s+-Werror\s+-O2\s*$",
              makefile, re.MULTILINE) is not None,
    "default Makefile build treats compiler warnings as errors",
)
require(
    'sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2 \\' in launcher,
    "interactive launcher uses the same strict compiler policy",
)
require(
    'TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"' in launcher
    and '-o "$TEMP_EXEC" \\' in launcher
    and 'mv -f "$TEMP_EXEC" "$R2_EXEC"' in launcher
    and '-o "$R2_EXEC" \\' not in launcher,
    "interactive build preserves the last known-good executable until atomic replacement",
)
require(
    'make clean all CFLAGS="-std=c11 -Wall -Wextra -Werror -O2"' in workflow,
    "CI compiles with the same strict compiler policy",
)
require(
    "      - 'release/**'" in workflow,
    "release branches run the same full CI workflow instead of silently skipping validation",
)
pull_request_block = re.search(
    r"^  pull_request:\n(?P<body>.*?)(?=^  [A-Za-z_][A-Za-z0-9_-]*:|\Z)",
    workflow,
    re.MULTILINE | re.DOTALL,
)
require(
    pull_request_block is not None and "      - 'release/**'" in pull_request_block.group("body"),
    "pull requests targeting release branches trigger the full CI workflow",
)
require(
    "python3 -m compileall -q ." in workflow,
    "CI syntax-checks every Python source and test module before targeted tests run",
)
require(
    'ROOT="$(mktemp -d /tmp/r2-log-linked-retrieval.' in workflow
    and '"$ROOT/log-linked-retrieval-test"' in workflow
    and 'sudo chmod -R a+rwx /home/x/R2_Home' not in workflow
    and 'sudo rm -rf /home/x/R2_Home' not in workflow,
    "Life Log integration test uses an isolated temporary root and never chmods/deletes a shared home path",
)
require(
    "python3 tests/copy_ollama_model_integrity.py" in workflow,
    "CI exercises repair of same-size corrupt model blobs and rejection of corrupt source blobs",
)
source_block = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.MULTILINE)
require(source_block is not None, "Makefile declares its production source list")
for source in required_sources:
    require(source in source_block.group(1), f"Makefile includes {source}")
    require(f'"$R2_SOURCE/{source}"' in launcher, f"launcher includes {source}")
require(
    "static const double HUNGER_PER_SECOND = 100.0 / (3.0 * 3600.0);" in reality
    and "double hunger = sqlite3_column_double(st, 0) - elapsed * HUNGER_PER_SECOND;" in reality
    and "hunger=MIN(100,hunger+?)" in reality,
    "fullness meter drains over three hours and eating restores it",
)
require(
    "static const double SATIETY_PER_SECOND = 0.1 / 4.75;" in reality
    and "double satisfaction = sqlite3_column_double(st, 4) - elapsed * SATIETY_PER_SECOND;" in reality,
    "satisfaction independently decays by 0.1 per 4.75 seconds",
)
require(
    "return valid_hunger && hunger < 50.0;" in core
    and "if (hunger < 50.0) {" in core
    and "If the fullness meter is below 50/100" in core,
    "model and autonomous feeding thresholds match decreasing fullness semantics",
)
require(
    "hunger_fullness_semantics_v2" in reality
    and "100-hunger" in reality,
    "legacy hunger values migrate to the new fullness scale exactly once",
)
bootstrap_bytes = (ROOT / "tools/r2_install_root_bootstrap.sh").read_bytes()
git_blob_sha = hashlib.sha1(b"blob " + str(len(bootstrap_bytes)).encode("ascii") + bytes([0]) + bootstrap_bytes).hexdigest()
pinned_bootstrap_sha = re.search(r"^BOOTSTRAP_BLOB_SHA=([0-9a-f]{40})$", overnight_docs, re.MULTILINE)
require(
    pinned_bootstrap_sha is not None and pinned_bootstrap_sha.group(1) == git_blob_sha,
    "one-time trusted bootstrap pin matches the exact reviewed source blob",
)
print("R2 build configuration smoke test passed.")
