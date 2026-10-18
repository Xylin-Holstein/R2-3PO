#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BOOTSTRAP="$ROOT/tools/r2_install_root_bootstrap.sh"
WRAPPER="$ROOT/tools/install_r2_overnight.sh"
RUNNER="$ROOT/tools/r2_service_runner.sh"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

bash -n "$BOOTSTRAP" || fail "trusted bootstrap has a shell syntax error."
bash -n "$WRAPPER" || fail "source-tree notice has a shell syntax error."
bash -n "$RUNNER" || fail "overnight service runner has a shell syntax error."
[[ -x "$WRAPPER" ]] || fail "source-tree notice lost its executable bit in the Git tree."

# The privileged entrypoint must only run from the protected root-owned path.
grep -Fq 'BOOTSTRAP_PATH="/usr/local/sbin/r2-3po-install"' "$BOOTSTRAP" ||
    fail "bootstrap no longer pins its trusted installed path."
grep -Fq 'refusing privileged execution outside the root-owned bootstrap path' "$BOOTSTRAP" ||
    fail "bootstrap no longer refuses execution from the mutable checkout."
grep -Fq '[[ "$EUID" -eq 0 ]]' "$BOOTSTRAP" ||
    fail "bootstrap no longer requires root execution."
grep -Fq 'runuser -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2' "$BOOTSTRAP" ||
    fail "trusted bootstrap no longer drops to the r2 account for mutable build/model/database operations."
grep -Fq 'run_as_r2 test -x "$path"' "$BOOTSTRAP" ||
    fail "bootstrap no longer verifies workspace traversal as r2."
grep -Fq 'run_as_r2 test -r "$path"' "$BOOTSTRAP" ||
    fail "bootstrap no longer verifies workspace readability as r2."
grep -Fq 'run_as_r2 test -w "$path"' "$BOOTSTRAP" ||
    fail "bootstrap no longer verifies workspace writability as r2."
grep -Fq 'root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-ollama.service"' "$BOOTSTRAP" ||
    fail "bootstrap no longer installs the embedded trusted Ollama unit."
grep -Fq 'root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-3po.service"' "$BOOTSTRAP" ||
    fail "bootstrap no longer installs the embedded trusted R2 unit."

# The source-tree wrapper is never a privileged deployment entrypoint.
grep -Fq 'do not execute this mutable source-tree script as root' "$WRAPPER" ||
    fail "source-tree notice no longer warns against privileged execution."
grep -Fq '/usr/local/sbin/r2-3po-install' "$WRAPPER" ||
    fail "source-tree notice no longer points to the protected bootstrap."
grep -Fq 'docs/overnight-service.md' "$WRAPPER" ||
    fail "source-tree notice no longer points to the immutable bootstrap/hash verification procedure."
! grep -Fq '/home/x/R2_Home/R2/tools/r2_install_root_bootstrap.sh /usr/local/sbin/r2-3po-install' "$WRAPPER" ||
    fail "source-tree notice still recommends privileged installation from the runtime-writable tree."
! grep -Fq 'root_run ' "$WRAPPER" ||
    fail "source-tree notice contains privileged deployment operations."

# The long-running service must independently re-check the same persistent
# paths on every start, including the diary and existing database files.
grep -Fq '[[ -w "$R2_ROOT" ]]' "$RUNNER" ||
    fail "service runner no longer checks R2 root write access."
grep -Fq '[[ -w "$R2_SOURCE" ]]' "$RUNNER" ||
    fail "service runner no longer checks source/database write access."
grep -Fq '[[ -d "$R2_ROOT/R2_Diary" && -w "$R2_ROOT/R2_Diary" ]]' "$RUNNER" ||
    fail "service runner no longer checks diary write access."
grep -Fq 'refusing symlinked R2 database' "$RUNNER" ||
    fail "service runner no longer refuses symlinked database paths."
grep -Fq 'existing database is not a readable/writable regular file' "$RUNNER" ||
    fail "service runner no longer validates existing database permissions."

echo "Trusted bootstrap and overnight permission/readiness regression checks passed."
