#!/usr/bin/env bash
# Build R2 and install/activate its supervised overnight systemd service.
set -euo pipefail

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
UNIT_SOURCE="$R2_SOURCE/deploy/r2-3po.service"
RUNNER_SOURCE="$R2_SOURCE/tools/r2_service_runner.sh"
UNIT_DEST="/etc/systemd/system/r2-3po.service"
TEMP_EXEC="${R2_ROOT}/r2.new.${BASHPID}"

log() { printf '[R2 installer] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit 1; }
if [[ "$EUID" -eq 0 ]]; then ROOT_CMD=(); else ROOT_CMD=(sudo); fi
root_run() { "${ROOT_CMD[@]}" "$@"; }
run_as_r2() {
    if [[ "$EUID" -eq 0 ]]; then
        runuser -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 "$@"
    else
        sudo -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 "$@"
    fi
}

BACKUP_EXEC=""

wait_for_core_ready() {
    local since="$1"
    for attempt in $(seq 1 75); do
        if root_run systemctl is-active --quiet r2-3po.service &&
           root_run journalctl -u r2-3po.service --since "$since" -o cat --no-pager 2>/dev/null |
               grep -Fq "[R2 READY] Core initialized; entering shell."; then
            return 0
        fi
        if root_run systemctl is-failed --quiet r2-3po.service; then
            break
        fi
        sleep 1
    done
    return 1
}

rollback_previous_executable() {
    # Stop any failed/new instance before inspecting or restoring the prior binary.
    root_run systemctl stop r2-3po.service >/dev/null 2>&1 || true
    if [[ -z "$BACKUP_EXEC" || ! -f "$BACKUP_EXEC" ]]; then
        log "WARNING: no previous executable is available to roll back to; the failed service has been stopped."
        return 1
    fi
    local restore_exec="${R2_ROOT}/r2.restore.${BASHPID}"
    if [[ -e "$restore_exec" || -L "$restore_exec" ]]; then
        log "ERROR: refusing to overwrite existing rollback path: $restore_exec"
        return 1
    fi
    if ! root_run cp "$BACKUP_EXEC" "$restore_exec" ||
       ! root_run chmod 0755 "$restore_exec" ||
       ! root_run chown r2:r2 "$restore_exec" ||
       ! root_run mv -f "$restore_exec" "$R2_EXEC"; then
        root_run rm -f "$restore_exec" || true
        log "ERROR: could not atomically restore the previous executable from $BACKUP_EXEC"
        return 1
    fi
    root_run chown r2:r2 "$R2_EXEC" || return 1
    root_run chmod 0755 "$R2_EXEC" || return 1
    log "Restored previous executable from $BACKUP_EXEC"
    root_run systemctl reset-failed r2-3po.service >/dev/null 2>&1 || true
    local rollback_started_at
    rollback_started_at="$(date '+%Y-%m-%d %H:%M:%S')"
    if ! root_run systemctl start r2-3po.service; then
        log "WARNING: previous executable is restored, but the service could not be started."
        return 1
    fi
    if wait_for_core_ready "$rollback_started_at"; then
        log "Previous executable restored and core readiness marker observed."
    else
        log "WARNING: previous executable is restored, but it did not reach core readiness."
        root_run systemctl --no-pager --full status r2-3po.service || true
        root_run journalctl -u r2-3po.service -n 100 --no-pager || true
        return 1
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

FAKETIME_FOUND=0
for candidate in \
    /usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1 \
    /usr/lib/x86_64-linux-gnu/libfaketime.so.1 \
    /usr/lib/faketime/libfaketime.so.1 \
    /usr/local/lib/faketime/libfaketime.so.1 \
    /usr/local/lib/libfaketime.so.1; do
    if [[ -r "$candidate" ]]; then FAKETIME_FOUND=1; break; fi
done
[[ "$FAKETIME_FOUND" -eq 1 ]] || fail "libfaketime.so.1 is missing. Install the libfaketime package before activation; do not update package indexes from this launcher."

if root_run systemctl is-active --quiet r2-3po.service 2>/dev/null; then
    fail "R2 service is already active. Stop it with 'sudo systemctl stop r2-3po' before rebuilding."
fi
if pgrep -x r2 >/dev/null 2>&1; then
    fail "an R2 process is already running. Shut it down before rebuilding so its executable and databases are not disturbed."
fi

# The existing design stores five SQLite databases inside R2/ alongside source code.
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
for database in "$R2_SOURCE/r2_memory.db" "$R2_SOURCE/r2_reality.db" "$R2_SOURCE/r2_fridge.db" "$R2_SOURCE/r2_rewards.db" "$R2_SOURCE/r2_addictions.db"; do
    [[ ! -L "$database" ]] || fail "refusing symlinked R2 database: $database"
    if [[ -e "$database" ]]; then
        run_as_r2 test -f "$database" && run_as_r2 test -r "$database" && run_as_r2 test -w "$database" ||
            fail "existing database is not a readable/writable regular file for r2: $database"
    fi
done

# No R2 process is running, so take a consistent, private snapshot of all five
# persistent databases and require a clean integrity audit before installing/starting code.
AUDIT_SCRIPT="$R2_SOURCE/tools/audit_r2_databases.py"
[[ -f "$AUDIT_SCRIPT" ]] || fail "database audit tool is missing: $AUDIT_SCRIPT"
SNAPSHOT_DIR="$R2_ROOT/audit-snapshot-$(date '+%Y%m%d-%H%M%S')"
log "Creating a private SQLite snapshot and checking database integrity..."
run_as_r2 python3 "$AUDIT_SCRIPT" --root "$R2_SOURCE" --snapshot-dir "$SNAPSHOT_DIR" ||
    fail "database snapshot/integrity audit failed. R2 was not activated; inspect the report and preserve the snapshot/source databases."
log "Database snapshot and integrity audit passed: $SNAPSHOT_DIR"

# Compile to a new path first. A failed build never replaces the last known executable.
# Make's clean target removes TARGET, so never let it remove an unexpected stale
# file or symlink that happens to collide with a reused process ID.
if [[ -e "$TEMP_EXEC" || -L "$TEMP_EXEC" ]]; then
    fail "refusing existing temporary build path: $TEMP_EXEC"
fi
log "Compiling the complete C runtime..."
make -C "$R2_SOURCE" clean all TARGET="$TEMP_EXEC"
[[ -x "$TEMP_EXEC" ]] || fail "build completed without producing $TEMP_EXEC."
chmod 0755 "$TEMP_EXEC"
root_run chown r2:r2 "$TEMP_EXEC"

# Preserve the last known executable before installing the new build. Refuse
# symlinks and unexpected file types rather than backing up an ambiguous target.
if [[ -L "$R2_EXEC" ]]; then
    fail "refusing symlinked existing executable: $R2_EXEC"
fi
if [[ -e "$R2_EXEC" ]]; then
    [[ -f "$R2_EXEC" && -x "$R2_EXEC" ]] ||
        fail "existing executable is not a regular executable file: $R2_EXEC"
    BACKUP_EXEC="$(mktemp "$R2_ROOT/r2.previous.XXXXXX")" ||
        fail "could not reserve a private rollback copy path."
    if ! cp "$R2_EXEC" "$BACKUP_EXEC"; then
        rm -f "$BACKUP_EXEC"
        BACKUP_EXEC=""
        fail "could not preserve the previous executable; existing installation was left untouched."
    fi
    chmod 0755 "$BACKUP_EXEC"
    root_run chown r2:r2 "$BACKUP_EXEC"
    log "Previous executable preserved at $BACKUP_EXEC"
fi

mv -f "$TEMP_EXEC" "$R2_EXEC"
root_run chown r2:r2 "$R2_EXEC"
root_run chmod 0755 "$R2_EXEC"

# Install only the launcher and unit. The FX collection stays on the host at R2_sounds/FX.
chmod 0755 "$RUNNER_SOURCE"
root_run install -m 0644 "$UNIT_SOURCE" "$UNIT_DEST"
root_run systemctl daemon-reload

log "Enabling R2 at boot and starting the overnight service..."
STARTED_AT="$(date '+%Y-%m-%d %H:%M:%S')"
if ! root_run systemctl enable --now r2-3po.service; then
    root_run systemctl --no-pager --full status r2-3po.service || true
    root_run journalctl -u r2-3po.service -n 100 --no-pager || true
    rollback_previous_executable || log "WARNING: automatic executable rollback/restart was incomplete."
    fail "systemd could not start R2. The previous executable was restored when available; fix the reported preflight/dependency issue before relying on overnight operation."
fi

# Type=simple becomes active before ExecStart finishes its dependency checks.
# Require a core-emitted readiness marker as well as active systemd state.
if ! wait_for_core_ready "$STARTED_AT"; then
    root_run systemctl --no-pager --full status r2-3po.service || true
    root_run journalctl -u r2-3po.service -n 100 --no-pager || true
    rollback_previous_executable || log "WARNING: automatic executable rollback/restart was incomplete."
    fail "R2 did not reach active state. The previous executable was restored when available; inspect the service logs before retrying deployment."
fi

log "R2 core initialization was observed; service is active and enabled for boot."
root_run systemctl --no-pager --full status r2-3po.service || true
log "Live logs: journalctl -u r2-3po.service -f"
log "Stop cleanly: sudo systemctl stop r2-3po.service"
