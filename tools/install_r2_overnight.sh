#!/usr/bin/env bash
# Build R2 and install/activate its supervised overnight systemd service.
set -euo pipefail

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
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

# Use the shell's built-in file tests when checking permissions as r2.
# On some hosts, invoking /usr/bin/test directly can disagree with a shell
# permission test even though r2 can actually create files in the directory.
r2_test() {
    local mode="$1" path="$2"
    run_as_r2 /bin/sh -c '
        case "$1" in
            -d) [ -d "$2" ] ;;
            -f) [ -f "$2" ] ;;
            -r) [ -r "$2" ] ;;
            -w) [ -w "$2" ] ;;
            -x) [ -x "$2" ] ;;
            *) exit 2 ;;
        esac
    ' sh "$mode" "$path"
}

# A real create/remove probe is the authoritative check for a directory
# that must hold SQLite journals, WAL files, diary entries, and runtime state.
r2_can_write_dir() {
    local path="$1"
    run_as_r2 /bin/sh -c '
        [ -d "$1" ] || exit 1
        probe=$(mktemp "$1/.r2-write-check.XXXXXX") || exit 1
        rm -f -- "$probe" || exit 1
    ' sh "$path"
}

latest_life_log_event_id() {
    run_as_r2 python3 -c '
import sqlite3, sys
path = sys.argv[1]
try:
    with sqlite3.connect("file:" + path + "?mode=ro", uri=True, timeout=2) as db:
        row = db.execute("SELECT COALESCE(MAX(id), 0) FROM r2_log_events").fetchone()
        print(int(row[0] or 0))
except sqlite3.Error:
    print(0)
' "$R2_SOURCE/r2_memory.db"
}

core_online_after() {
    local prior_id="$1"
    run_as_r2 python3 -c '
import sqlite3, sys
path, prior_id = sys.argv[1], int(sys.argv[2])
try:
    with sqlite3.connect("file:" + path + "?mode=ro", uri=True, timeout=2) as db:
        row = db.execute(
            "SELECT 1 FROM r2_log_events WHERE event_type = ? AND id > ? LIMIT 1",
            ("core_online", prior_id)
        ).fetchone()
    sys.exit(0 if row else 1)
except (sqlite3.Error, ValueError):
    sys.exit(1)
' "$R2_SOURCE/r2_memory.db" "$prior_id"
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

# The existing design stores SQLite databases inside R2/ alongside source code.
# The root and source directory must already be writable; create missing data-only
# folders as r2 so first-run setup does not leave them owned by the desktop account.
for path in "$R2_ROOT" "$R2_SOURCE"; do
    [[ -d "$path" ]] || fail "required persistent-data directory is missing: $path"
    r2_test -x "$path" || fail "Linux user r2 cannot traverse $path; fix directory permissions before activation."
    r2_test -r "$path" || fail "Linux user r2 cannot read $path; fix permissions before activation."
    r2_can_write_dir "$path" || fail "Linux user r2 cannot create persistent files in $path; SQLite, memories, and diary need write access."
done
run_as_r2 mkdir -p \
    "$R2_ROOT/R2_Diary" \
    "$R2_ROOT/room/shelf" \
    "$R2_ROOT/room/box" \
    "$R2_ROOT/room/piggybank" \
    "$R2_ROOT/Pockets/Wallet" \
    "$R2_ROOT/fridge" || fail "could not create the runtime data directories as r2."
for path in "$R2_ROOT/R2_Diary" "$R2_ROOT/room" "$R2_ROOT/Pockets" "$R2_ROOT/fridge"; do
    r2_test -x "$path" && r2_test -r "$path" && r2_can_write_dir "$path" ||
        fail "Linux user r2 needs read/write/traverse access to $path before activation."
done
for database in "$R2_SOURCE/r2_memory.db" "$R2_SOURCE/r2_reality.db" "$R2_SOURCE/r2_fridge.db" "$R2_SOURCE/r2_rewards.db" "$R2_SOURCE/r2_addictions.db"; do
    [[ ! -L "$database" ]] || fail "refusing symlinked R2 database: $database"
    if [[ -e "$database" ]]; then
        r2_test -f "$database" && r2_test -r "$database" && r2_test -w "$database" ||
            fail "existing database is not a readable/writable regular file for r2: $database"
    fi
done

# No R2 process is running, so take a consistent, private snapshot of all five
# databases and require a clean integrity audit before installing/starting code.
AUDIT_SCRIPT="$R2_SOURCE/tools/audit_r2_databases.py"
[[ -f "$AUDIT_SCRIPT" ]] || fail "database audit tool is missing: $AUDIT_SCRIPT"
SNAPSHOT_DIR="$R2_ROOT/audit-snapshot-$(date '+%Y%m%d-%H%M%S')"
log "Creating a private SQLite snapshot and checking database integrity..."
run_as_r2 python3 "$AUDIT_SCRIPT" --root "$R2_SOURCE" --snapshot-dir "$SNAPSHOT_DIR" ||
    fail "database snapshot/integrity audit failed. R2 was not activated; inspect the report and preserve the snapshot/source databases."
log "Database snapshot and integrity audit passed: $SNAPSHOT_DIR"

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
STARTED_AT="$(date '+%Y-%m-%d %H:%M:%S')"
START_EVENT_ID="$(latest_life_log_event_id)" || fail "could not read the current Life Log position before startup."
if ! root_run systemctl enable --now r2-3po.service; then
    root_run systemctl --no-pager --full status r2-3po.service || true
    root_run journalctl -u r2-3po.service -n 100 --no-pager || true
    fail "systemd could not start R2. Fix the reported preflight/dependency issue before relying on overnight operation."
fi

# Type=simple becomes active before ExecStart finishes its dependency checks.
# The runner's clock message occurs before exec; require a new core_online Life Log
# event as well as an active service, proving R2 completed core initialization.
ACTIVE=0
for attempt in $(seq 1 75); do
    if root_run systemctl is-active --quiet r2-3po.service &&
       core_online_after "$START_EVENT_ID"; then
        ACTIVE=1
        break
    fi
    if root_run systemctl is-failed --quiet r2-3po.service; then
        break
    fi
    sleep 1
done
if [[ "$ACTIVE" -ne 1 ]]; then
    root_run systemctl --no-pager --full status r2-3po.service || true
    root_run journalctl -u r2-3po.service -n 100 --no-pager || true
    fail "R2 did not record a new core_online Life Log event while the service remained active. The unit may be enabled but the core has not been verified as initialized."
fi

log "R2 is active and enabled for boot."
root_run systemctl --no-pager --full status r2-3po.service || true
log "Live logs: journalctl -u r2-3po.service -f"
log "Stop cleanly: sudo systemctl stop r2-3po.service"
