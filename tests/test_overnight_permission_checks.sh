#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALLER="$ROOT/tools/install_r2_overnight.sh"
RUNNER="$ROOT/tools/r2_service_runner.sh"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

bash -n "$INSTALLER" || fail "overnight installer has a shell syntax error."
bash -n "$RUNNER" || fail "overnight service runner has a shell syntax error."
[[ -x "$INSTALLER" ]] || fail "installer lost its executable bit in the Git tree."

# The installer must prove that r2 can traverse, read, and write the canonical
# workspace before it modifies or activates the overnight service.
grep -Fq 'run_as_r2 test -x "$path"' "$INSTALLER" ||
    fail "installer no longer verifies workspace traversal as r2."
grep -Fq 'run_as_r2 test -r "$path"' "$INSTALLER" ||
    fail "installer no longer verifies workspace readability as r2."
grep -Fq 'run_as_r2 test -w "$path"' "$INSTALLER" ||
    fail "installer no longer verifies workspace writability as r2."
grep -Fq 'runuser -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2' "$INSTALLER" ||
    fail "root-run installer no longer drops to the r2 account for access checks."
grep -Fq 'sudo -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2' "$INSTALLER" ||
    fail "unprivileged installer no longer checks access through sudo as r2."

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

echo "Overnight installer permission/readiness regression checks passed."
