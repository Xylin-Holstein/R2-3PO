#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALLER="$ROOT/tools/install_r2_overnight.sh"
RUNNER="$ROOT/tools/r2_service_runner.sh"

bash -n "$INSTALLER"
bash -n "$RUNNER"
if [[ ! -x "$INSTALLER" ]]; then
    echo "ERROR: installer lost its executable bit in the Git tree." >&2
    exit 1
fi

grep -Fq 'r2_can_write_dir "$path"' "$INSTALLER"
grep -Fq 'core_online_after "$START_EVENT_ID"' "$INSTALLER"
grep -Fq 'directory_write_probe "$R2_ROOT"' "$RUNNER"
grep -Fq 'directory_write_probe "$R2_SOURCE"' "$RUNNER"
grep -Fq 'directory_write_probe "$R2_ROOT/R2_Diary"' "$RUNNER"

if grep -Eq 'run_as_r2[[:space:]]+test([[:space:]]|$)' "$INSTALLER"; then
    echo "ERROR: installer still invokes external test through run_as_r2." >&2
    exit 1
fi
if grep -Eq 'sudo -u r2[[:space:]]+test[[:space:]]+-w' "$ROOT/docs/overnight-service.md"; then
    echo "ERROR: service guide still documents the unreliable external write check." >&2
    exit 1
fi

# Exercise the actual directory-probe functions against a disposable directory.
tmpdir="$(mktemp -d)"
cleanup() { rm -rf -- "$tmpdir"; }
trap cleanup EXIT

run_as_r2() { "$@"; }
eval "$(sed -n '/^r2_can_write_dir() {$/,/^}$/p' "$INSTALLER")"
r2_can_write_dir "$tmpdir"
if find "$tmpdir" -maxdepth 1 -name '.r2-write-check.*' -print -quit | grep -q .; then
    echo "ERROR: installer directory probe left a temporary file behind." >&2
    exit 1
fi

eval "$(sed -n '/^directory_write_probe() {$/,/^}$/p' "$RUNNER")"
directory_write_probe "$tmpdir"
if find "$tmpdir" -maxdepth 1 -name '.r2-write-check.*' -print -quit | grep -q .; then
    echo "ERROR: service runner directory probe left a temporary file behind." >&2
    exit 1
fi

echo "Overnight installer permission/readiness regression checks passed."
