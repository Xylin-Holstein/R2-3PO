#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

R2_HOME="$TMP_ROOT" bash "$SOURCE_DIR/install_tv.sh"

TV_DIR="$TMP_ROOT/room/TV"
test -x "$TV_DIR/TV.py"
test -x "$TV_DIR/run_tv.sh"
test -x "$TV_DIR/TV.desktop"
test -d "$TMP_ROOT/VCR_Tapes"
grep -Fq "R2 Reality" "$TV_DIR/device.txt"
grep -Fq "$TMP_ROOT/R2/r2_reality.db" "$TV_DIR/device.txt"
grep -Fq "$TV_DIR/TV.py" "$TV_DIR/run_tv.sh"
grep -Fq "Exec=/bin/bash" "$TV_DIR/TV.desktop"
grep -Fq 'sudo -u r2 env -u XAUTHORITY' "$TV_DIR/run_tv.sh"
grep -Fq 'DISPLAY="${DISPLAY:-}"' "$TV_DIR/run_tv.sh"
grep -Fq 'R2_TV_SOCKET="$R2_TV_SOCKET"' "$TV_DIR/run_tv.sh"
grep -Fq 'Terminal=true' "$TV_DIR/TV.desktop"

echo "TV installer smoke test passed."
