#!/usr/bin/env bash
# Build R2 and install/activate its supervised overnight systemd service.
set -euo pipefail

R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"
R2_SOURCE="${R2_SOURCE:-$R2_ROOT/R2}"
R2_EXEC="${R2_EXEC:-$R2_ROOT/r2}"
UNIT_SOURCE="$R2_SOURCE/deploy/r2-3po.service"
RUNNER_SOURCE="$R2_SOURCE/tools/r2_service_runner.sh"
UNIT_DEST="/etc/systemd/system/r2-3po.service"
TEMP_EXEC="$R2_ROOT/r2.new"

log() { printf '[R2 installer] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit 1; }
if [[ "$EUID" -eq 0 ]]; then ROOT_CMD=(); else ROOT_CMD=(sudo); fi
root_run() { "${ROOT_CMD[@]}" "$@"; }
run_as_r2() {
    if [[ "$EUID" -eq 0 ]]; then
        runuser -u r2 -- "$@"
    else
        sudo -u r2 -- "$@"
    fi
}

[[ -d "$R2_SOURCE" ]] || fail "expected source tree at $R2_SOURCE. Copy/clone this repository there first, then rerun."
[[ -f "$R2_SOURCE/Makefile" && -f "$UNIT_SOURCE" && -f "$RUNNER_SOURCE" ]] || fail "repository files are incomplete; expected Makefile, deploy/r2-3po.service, and tools/r2_service_runner.sh."
id r2 >/dev/null 2>&1 || fail "Linux account 'r2' does not exist. Create the dedicated runtime account first."
command -v make >/dev/null 2>&1 || fail "make is missing."
command -v gcc >/dev/null 2>&1 || fail "gcc is missing."
command -v systemctl >/dev/null 2>&1 || fail "systemd/systemctl is required."
command -v curl >/dev/null 2>&1 || fail "curl is required."
command -v python3 >/dev/null 2>&1 || fail "python3 is required."

if root_run systemctl is-active --quiet r2-3po.service 2>/dev/null; then
    fail "R2 service is already active. Stop it with 'sudo systemctl stop r2-3po' before rebuilding."
fi
if pgrep -x r2 >/dev/null 2>&1; then
    fail "an R2 process is already running. Shut it down before rebuilding so its executable and databases are not disturbed."
fi

# The existing design stores SQLite databases inside R2/ alongside source code.
# The root and source directory must already be writable; create missing data-only
# folders as r2 so first-run setup does not leave them owned by the desktop account.
for path in "$R2_ROOT" "$R2_SOURCE"; do
    [[ -d "$path" ]] || fail "required persistent-data directory is missing: $path"
    run_as_r2 test -x "$path" || fail "Linux user r2 cannot traverse $path; fix directory permissions before activation."
    run_as_r2 test -r "$path" || fail "Linux user r2 cannot read $path; fix permissions before activation."
    run_as_r2 test -w "$path" || fail "Linux user r2 cannot write $path; SQLite, memories, and diary need persistent write access."
done
run_as_r2 mkdir -p \
    "$R2_ROOT/R2_Diary" \
    "$R2_ROOT/room/shelf" \
    "$R2_ROOT/room/box" \
    "$R2_ROOT/room/piggybank" \
    "$R2_ROOT/Pockets/Wallet" \
    "$R2_ROOT/fridge" || fail "could not create the runtime data directories as r2."
for path in "$R2_ROOT/R2_Diary" "$R2_ROOT/room" "$R2_ROOT/Pockets" "$R2_ROOT/fridge"; do
    run_as_r2 test -x "$path" && run_as_r2 test -r "$path" && run_as_r2 test -w "$path" ||
        fail "Linux user r2 needs read/write/traverse access to $path before activation."
done

# Compile to a new path first. A failed build never replaces the last known executable.
log "Compiling the complete C runtime..."
make -C "$R2_SOURCE" clean all TARGET="$TEMP_EXEC"
[[ -x "$TEMP_EXEC" ]] || fail "build completed without producing $TEMP_EXEC."
chmod 0755 "$TEMP_EXEC"
root_run chown r2:r2 "$TEMP_EXEC"
mv -f "$TEMP_EXEC" "$R2_EXEC"
root_run chown r2:r2 "$R2_EXEC"
chmod 0755 "$R2_EXEC"

# Install only the launcher and unit. The FX collection stays on the host at R2_sounds/FX.
chmod 0755 "$RUNNER_SOURCE"
root_run install -m 0644 "$UNIT_SOURCE" "$UNIT_DEST"
root_run systemctl daemon-reload

log "Enabling R2 at boot and starting the overnight service..."
root_run systemctl enable --now r2-3po.service
log "Service enable/start requested. Inspect it with: systemctl status r2-3po.service"
log "Live logs: journalctl -u r2-3po.service -f"
log "Stop cleanly: sudo systemctl stop r2-3po.service"
