#!/usr/bin/env bash
# TRUSTED ROOT BOOTSTRAP. Install this file once as root-owned
# /usr/local/sbin/r2-3po-install, then invoke it through the unprivileged wrapper.
# It never executes source-tree scripts or installs unit contents as root.
set -euo pipefail

# Do not let an inherited PATH select attacker-controlled utilities for root
# filesystem/service operations. Keep the same known system path for child tools.
PATH="/usr/sbin:/usr/bin:/sbin:/bin"
export PATH
umask 077

[[ "$EUID" -eq 0 ]] || {
    echo "[R2 installer] ERROR: this trusted bootstrap must be invoked as root via sudo." >&2
    exit 1
}

# Even this file exists in the mutable checkout before its one-time installation.
# Refuse to run that copy with privilege; only the protected installed path is valid.
BOOTSTRAP_PATH="/usr/local/sbin/r2-3po-install"
if [[ "$0" != "$BOOTSTRAP_PATH" || -L "$BOOTSTRAP_PATH" ||
      ! -f "$BOOTSTRAP_PATH" || "$(stat -c '%u' "$BOOTSTRAP_PATH")" != "0" ]]; then
    echo "[R2 installer] ERROR: refusing privileged execution outside the root-owned bootstrap path." >&2
    exit 1
fi
BOOTSTRAP_MODE="$(stat -c '%a' "$BOOTSTRAP_PATH")"
if (( (8#$BOOTSTRAP_MODE & 022) != 0 )); then
    echo "[R2 installer] ERROR: installed bootstrap must not be group/world-writable." >&2
    exit 1
fi

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
RUNNER_SOURCE="$R2_SOURCE/tools/r2_service_runner.sh"
OLLAMA_RUNNER_SOURCE="$R2_SOURCE/tools/r2_ollama_runner.sh"
MODEL_COPY_SCRIPT="$R2_SOURCE/tools/copy_ollama_model.py"
R2_MODEL="qwen3.5:9b"
R2_OLLAMA_BASE_URL="http://127.0.0.1:11435"
UNIT_DEST="/etc/systemd/system/r2-3po.service"
OLLAMA_UNIT_DEST="/etc/systemd/system/r2-ollama.service"
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
INSTALL_SUCCEEDED=0
NEW_EXEC_INSTALLED=0
OLLAMA_STATE_CAPTURED=0
OLLAMA_WAS_ACTIVE=0
OLLAMA_WAS_ENABLED=0
R2_ROLLBACK_RESTARTED=0
UNIT_STAGING_DIR=""

cleanup_unit_staging() {
    if [[ -n "${UNIT_STAGING_DIR:-}" && -d "$UNIT_STAGING_DIR" ]]; then
        rm -rf -- "$UNIT_STAGING_DIR" ||
            log "WARNING: could not remove temporary unit staging directory: $UNIT_STAGING_DIR"
    fi
}

cleanup_on_exit() {
    local status=$?
    trap - EXIT
    cleanup_unit_staging

    # Cover failures after replacing R2 but before the explicit activation
    # rollback path (for example, chmod/date/systemd setup failures).
    if [[ "$status" -ne 0 && "$INSTALL_SUCCEEDED" != "1" &&
          "$NEW_EXEC_INSTALLED" == "1" ]]; then
        if [[ -n "$BACKUP_EXEC" && -f "$BACKUP_EXEC" ]]; then
            local restore_exec=""
            restore_exec="$(run_as_r2 mktemp "$R2_ROOT/r2.restore.XXXXXX" 2>/dev/null)" || true
            if [[ -n "$restore_exec" ]] &&
               run_as_r2 cp "$BACKUP_EXEC" "$restore_exec" &&
               run_as_r2 chmod 0755 "$restore_exec" &&
               run_as_r2 mv -f "$restore_exec" "$R2_EXEC"; then
                NEW_EXEC_INSTALLED=0
                log "Restored previous executable from $BACKUP_EXEC after an early installer failure."
            else
                [[ -z "$restore_exec" ]] || run_as_r2 rm -f "$restore_exec" || true
                log "ERROR: automatic early-exit rollback could not restore $BACKUP_EXEC."
                root_run systemctl stop r2-3po.service >/dev/null 2>&1 || true
            fi
        else
            log "ERROR: installer failed after replacing R2, but no prior executable exists to restore."
            root_run systemctl stop r2-3po.service >/dev/null 2>&1 || true
            root_run systemctl disable r2-3po.service >/dev/null 2>&1 || true
            run_as_r2 rm -f -- "$R2_EXEC" ||
                log "ERROR: could not remove the failed first-install executable: $R2_EXEC"
            NEW_EXEC_INSTALLED=0
        fi
    fi
    if [[ -e "$TEMP_EXEC" || -L "$TEMP_EXEC" ]]; then
        run_as_r2 rm -f -- "$TEMP_EXEC" || true
    fi

    # The installer stops the private model server while repairing its model
    # store. If any pre-activation step fails, restore its original enabled and
    # active state instead of leaving a previously running service stranded.
    # If rollback successfully restarted the previous R2 core, keep its model
    # server running; that core depends on it.
    if [[ "$INSTALL_SUCCEEDED" != "1" &&
          "$OLLAMA_STATE_CAPTURED" == "1" &&
          "$R2_ROLLBACK_RESTARTED" != "1" ]]; then
        if [[ "$OLLAMA_WAS_ENABLED" == "1" ]]; then
            root_run systemctl enable r2-ollama.service >/dev/null 2>&1 || true
        else
            root_run systemctl disable r2-ollama.service >/dev/null 2>&1 || true
        fi
        if [[ "$OLLAMA_WAS_ACTIVE" == "1" ]]; then
            root_run systemctl start r2-ollama.service >/dev/null 2>&1 ||
                log "WARNING: could not restore the private Ollama service's prior active state."
        else
            root_run systemctl stop r2-ollama.service >/dev/null 2>&1 || true
        fi
    fi
    exit "$status"
}
trap cleanup_on_exit EXIT

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
    if ! run_as_r2 cp "$BACKUP_EXEC" "$restore_exec" ||
       ! run_as_r2 chmod 0755 "$restore_exec" ||
       ! run_as_r2 mv -f "$restore_exec" "$R2_EXEC"; then
        run_as_r2 rm -f "$restore_exec" || true
        log "ERROR: could not atomically restore the previous executable from $BACKUP_EXEC"
        return 1
    fi
    NEW_EXEC_INSTALLED=0
    log "Restored previous executable from $BACKUP_EXEC"
    root_run systemctl reset-failed r2-3po.service >/dev/null 2>&1 || true
    local rollback_started_at
    rollback_started_at="$(date '+%Y-%m-%d %H:%M:%S')"
    if ! root_run systemctl start r2-3po.service; then
        log "WARNING: previous executable is restored, but the service could not be started."
        return 1
    fi
    if wait_for_core_ready "$rollback_started_at"; then
        R2_ROLLBACK_RESTARTED=1
        log "Previous executable restored and core readiness marker observed."
    else
        log "WARNING: previous executable is restored, but it did not reach core readiness."
        root_run systemctl --no-pager --full status r2-3po.service || true
        root_run journalctl -u r2-3po.service -n 100 --no-pager || true
        return 1
    fi
}

[[ -d "$R2_SOURCE" ]] || fail "expected source tree at $R2_SOURCE. Copy/clone this repository there first, then rerun."
[[ -f "$R2_SOURCE/Makefile" && -f "$RUNNER_SOURCE" && -f "$OLLAMA_RUNNER_SOURCE" && -f "$MODEL_COPY_SCRIPT" ]] || fail "repository files are incomplete; expected Makefile, both runners, and the model-copy tool."
id r2 >/dev/null 2>&1 || fail "Linux account 'r2' does not exist. Create the dedicated runtime account first."
command -v make >/dev/null 2>&1 || fail "make is missing."
command -v gcc >/dev/null 2>&1 || fail "gcc is missing."
command -v systemctl >/dev/null 2>&1 || fail "systemd/systemctl is required."
command -v curl >/dev/null 2>&1 || fail "curl is required."
command -v ollama >/dev/null 2>&1 || fail "ollama is required to run R2-owned inference."
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

# Record the private Ollama service's pre-install state before any stop/enable
# operation. A failed model copy/build/preflight must not strand an existing
# service in a different state from the one the operator had before installing.
OLLAMA_ACTIVE_STATE="$(root_run systemctl is-active r2-ollama.service 2>/dev/null || true)"
case "$OLLAMA_ACTIVE_STATE" in
    active) OLLAMA_WAS_ACTIVE=1 ;;
    inactive|failed|unknown) OLLAMA_WAS_ACTIVE=0 ;;
    *) fail "could not establish the private Ollama service's prior active state; refusing installation." ;;
esac
OLLAMA_ENABLED_STATE="$(root_run systemctl is-enabled r2-ollama.service 2>/dev/null || true)"
case "$OLLAMA_ENABLED_STATE" in
    enabled|enabled-runtime|linked|linked-runtime) OLLAMA_WAS_ENABLED=1 ;;
    disabled|static|indirect|masked|masked-runtime|generated|transient|alias|not-found) OLLAMA_WAS_ENABLED=0 ;;
    *) fail "could not establish the private Ollama service's prior enabled state; refusing installation." ;;
esac
OLLAMA_STATE_CAPTURED=1

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
    "$R2_ROOT/fridge" \
    "$R2_ROOT/.ollama/models" || fail "could not create the runtime data directories as r2."
for path in "$R2_ROOT/R2_Diary" "$R2_ROOT/room" "$R2_ROOT/Pockets" "$R2_ROOT/fridge" "$R2_ROOT/.ollama" "$R2_ROOT/.ollama/models"; do
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

# Keep the desktop user's Ollama service untouched. Copy only the already
# installed model into the dedicated r2-owned model store; never download it.
root_run systemctl stop r2-ollama.service >/dev/null 2>&1 || true
OLLAMA_UNIT_STATE="$(root_run systemctl is-active r2-ollama.service 2>/dev/null || true)"
case "$OLLAMA_UNIT_STATE" in
    inactive|failed|unknown) ;;
    active|activating|deactivating|reloading)
        fail "the private Ollama service remains active or transitioning after the stop request; refusing to mutate its live model store."
        ;;
    *)
        fail "could not verify that the private Ollama service stopped; refusing to mutate its model store."
        ;;
esac
if root_run pgrep -u r2 -x ollama >/dev/null 2>&1; then
    fail "an r2-owned Ollama process remains after the stop request; refusing to mutate its live model store."
else
    OLLAMA_PGREP_STATUS=$?
    [[ "$OLLAMA_PGREP_STATUS" -eq 1 ]] ||
        fail "could not verify that no r2-owned Ollama process remains; refusing to mutate its model store."
fi
log "Copying the already-installed $R2_MODEL into R2's private model store..."
# The model store belongs to r2. Run the copier without privilege: the source
# script and destination are both in r2-writable locations, so root must never
# execute this mutable Python file. Source model blobs are expected to be readable
# to the desktop account/runtime user; fail closed if host permissions deny that.
run_as_r2 python3 "$MODEL_COPY_SCRIPT" --model "$R2_MODEL" \
    --destination-root "$R2_ROOT/.ollama/models" --owner r2 ||
    fail "could not prepare R2's private model store. Ensure $R2_MODEL is installed in the existing local Ollama store; no external download was attempted."

# Compile to a new path first. A failed build never replaces the last known executable.
# Make's clean target removes TARGET, so never let it remove an unexpected stale
# file or symlink that happens to collide with a reused process ID.
if [[ -e "$TEMP_EXEC" || -L "$TEMP_EXEC" ]]; then
    fail "refusing existing temporary build path: $TEMP_EXEC"
fi
log "Compiling the complete C runtime..."
# The Makefile and all C sources live in the runtime-writable tree. Compile
# as r2 so a modified Makefile/source can never execute build commands as root.
run_as_r2 make -C "$R2_SOURCE" clean all TARGET="$TEMP_EXEC"
[[ -x "$TEMP_EXEC" ]] || fail "build completed without producing $TEMP_EXEC."
run_as_r2 chmod 0755 "$TEMP_EXEC"

# Keep the newly built executable at TEMP_EXEC until the private Ollama
# service has passed its real inference preflight. A model-server failure must
# not replace the last known executable.

# Install both isolated model and R2 units. The FX collection stays on the host at R2_sounds/FX.
# These runner files live in the runtime-writable tree. Never chmod them as root:
# chmod follows symlinks, so a compromised r2 account could redirect the operation
# onto an unrelated host file. The r2 account owns this workspace and can set its
# own executable bits without privilege.
run_as_r2 chmod 0755 "$RUNNER_SOURCE" "$OLLAMA_RUNNER_SOURCE"

# Generate systemd units from this trusted, root-owned bootstrap. Never read
# privileged unit content from R2_SOURCE: that tree must remain writable by r2.
UNIT_STAGING_DIR="$(mktemp -d /run/r2-3po-install.XXXXXX)" ||
    fail "could not create a private root-owned unit staging directory."
chmod 0700 "$UNIT_STAGING_DIR"
cat > "$UNIT_STAGING_DIR/r2-ollama.service" <<'R2_OLLAMA_UNIT_EOF'
[Unit]
Description=R2-3PO isolated Ollama inference server (runs as r2)
Documentation=https://github.com/Xylin-Holstein/R2-3PO
After=network.target
Before=r2-3po.service
StartLimitIntervalSec=0

[Service]
Type=simple
User=r2
Group=r2
SupplementaryGroups=video render
WorkingDirectory=/home/x/R2_Home
Environment=HOME=/home/x/R2_Home
Environment=USER=r2
Environment=LOGNAME=r2
Environment=OLLAMA_HOST=127.0.0.1:11435
Environment=OLLAMA_MODELS=/home/x/R2_Home/.ollama/models
Environment=R2_MODEL=qwen3.5:9b
# Apply egress isolation before the model server starts. This rule remains active
# for as long as this service is running, including when the R2 conversation
# service stops. Only the two local Ollama ports are allowed: 11435 for this service and 11434 for the interactive launcher.
ExecStartPre=+/bin/sh -c 'set -eu; while /usr/sbin/iptables -w -C OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null; do /usr/sbin/iptables -w -D OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK; done; /usr/sbin/iptables -w -N R2_OLLAMA_NETLOCK 2>/dev/null || true; /usr/sbin/iptables -w -F R2_OLLAMA_NETLOCK; /usr/sbin/iptables -w -A R2_OLLAMA_NETLOCK -o lo -d 127.0.0.1/32 -p tcp --dport 11435 -j ACCEPT;  /usr/sbin/iptables -w -A R2_OLLAMA_NETLOCK -o lo -d 127.0.0.1/32 -p tcp --dport 11434 -j ACCEPT;/usr/sbin/iptables -w -A R2_OLLAMA_NETLOCK -m conntrack --ctstate ESTABLISHED -o lo -s 127.0.0.1/32 -p tcp --sport 11435 -j ACCEPT;  /usr/sbin/iptables -w -A R2_OLLAMA_NETLOCK -m conntrack --ctstate ESTABLISHED -o lo -s 127.0.0.1/32 -p tcp --sport 11434 -j ACCEPT;/usr/sbin/iptables -w -A R2_OLLAMA_NETLOCK -j REJECT; /usr/sbin/iptables -w -I OUTPUT 1 -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK'
ExecStartPre=+/bin/sh -c 'set -eu; while /usr/sbin/ip6tables -w -C OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null; do /usr/sbin/ip6tables -w -D OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK; done; /usr/sbin/ip6tables -w -N R2_OLLAMA_NETLOCK 2>/dev/null || true; /usr/sbin/ip6tables -w -F R2_OLLAMA_NETLOCK; /usr/sbin/ip6tables -w -A R2_OLLAMA_NETLOCK -j REJECT; /usr/sbin/ip6tables -w -I OUTPUT 1 -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK'
ExecStart=/home/x/R2_Home/R2/tools/r2_ollama_runner.sh
ExecStopPost=+/bin/sh -c 'while /usr/sbin/iptables -w -C OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null; do /usr/sbin/iptables -w -D OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null || break; done; /usr/sbin/iptables -w -F R2_OLLAMA_NETLOCK 2>/dev/null || true; /usr/sbin/iptables -w -X R2_OLLAMA_NETLOCK 2>/dev/null || true; while /usr/sbin/ip6tables -w -C OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null; do /usr/sbin/ip6tables -w -D OUTPUT -m owner --uid-owner r2 -j R2_OLLAMA_NETLOCK 2>/dev/null || break; done; /usr/sbin/ip6tables -w -F R2_OLLAMA_NETLOCK 2>/dev/null || true; /usr/sbin/ip6tables -w -X R2_OLLAMA_NETLOCK 2>/dev/null || true; exit 0'
Restart=on-failure
RestartSec=5
KillSignal=SIGTERM
TimeoutStopSec=90
UMask=0077
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
R2_OLLAMA_UNIT_EOF

cat > "$UNIT_STAGING_DIR/r2-3po.service" <<'R2_SERVICE_UNIT_EOF'
[Unit]
Description=R2-3PO overnight companion
Documentation=https://github.com/Xylin-Holstein/R2-3PO
Wants=network-online.target r2-ollama.service
After=network-online.target r2-ollama.service sound.target
BindsTo=r2-ollama.service
StartLimitIntervalSec=0

[Service]
Type=simple
User=r2
Group=r2
# Permit the service to use local ALSA playback/capture devices when present.
# This is service-scoped; it does not add the r2 account to audio globally.
SupplementaryGroups=audio video render
WorkingDirectory=/home/x/R2_Home
Environment=R2_ROOT=/home/x/R2_Home
Environment=HOME=/home/x/R2_Home
Environment=R2_SOURCE=/home/x/R2_Home/R2
Environment=R2_EXEC=/home/x/R2_Home/r2
Environment=R2_FX_DIR=/home/x/R2_Home/R2_sounds/FX
Environment=R2_SERVICE_MODE=1
Environment=R2_MODEL=qwen3.5:9b
Environment=R2_OLLAMA_BASE_URL=http://127.0.0.1:11435
# Per-UID egress isolation: only local Ollama TCP port 11435 is allowed for r2.
# These inline root commands avoid executing a mutable script from R2's
# runtime-writable source/data directory as root.
ExecStartPre=+/bin/sh -c 'set -eu; while /usr/sbin/iptables -w -C OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null; do /usr/sbin/iptables -w -D OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK; done; /usr/sbin/iptables -w -N R2_SVC_NETLOCK 2>/dev/null || true; /usr/sbin/iptables -w -F R2_SVC_NETLOCK; /usr/sbin/iptables -w -A R2_SVC_NETLOCK -o lo -d 127.0.0.1/32 -p tcp --dport 11435 -j ACCEPT; /usr/sbin/iptables -w -A R2_SVC_NETLOCK -m conntrack --ctstate ESTABLISHED -o lo -s 127.0.0.1/32 -p tcp --sport 11435 -j ACCEPT; /usr/sbin/iptables -w -A R2_SVC_NETLOCK -j REJECT; /usr/sbin/iptables -w -I OUTPUT 1 -m owner --uid-owner r2 -j R2_SVC_NETLOCK'
ExecStartPre=+/bin/sh -c 'set -eu; while /usr/sbin/ip6tables -w -C OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null; do /usr/sbin/ip6tables -w -D OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK; done; /usr/sbin/ip6tables -w -N R2_SVC_NETLOCK 2>/dev/null || true; /usr/sbin/ip6tables -w -F R2_SVC_NETLOCK; /usr/sbin/ip6tables -w -A R2_SVC_NETLOCK -j REJECT; /usr/sbin/ip6tables -w -I OUTPUT 1 -m owner --uid-owner r2 -j R2_SVC_NETLOCK'
ExecStart=/home/x/R2_Home/R2/tools/r2_service_runner.sh
# Always remove the temporary rules after service shutdown or failed startup.
ExecStopPost=+/bin/sh -c 'while /usr/sbin/iptables -w -C OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null; do /usr/sbin/iptables -w -D OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null || break; done; /usr/sbin/iptables -w -F R2_SVC_NETLOCK 2>/dev/null || true; /usr/sbin/iptables -w -X R2_SVC_NETLOCK 2>/dev/null || true; while /usr/sbin/ip6tables -w -C OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null; do /usr/sbin/ip6tables -w -D OUTPUT -m owner --uid-owner r2 -j R2_SVC_NETLOCK 2>/dev/null || break; done; /usr/sbin/ip6tables -w -F R2_SVC_NETLOCK 2>/dev/null || true; /usr/sbin/ip6tables -w -X R2_SVC_NETLOCK 2>/dev/null || true; exit 0'
Restart=always
RestartSec=10
KillSignal=SIGTERM
TimeoutStopSec=240
UMask=0077
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
R2_SERVICE_UNIT_EOF

root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-ollama.service" "$OLLAMA_UNIT_DEST"
root_run install -o root -g root -m 0644 "$UNIT_STAGING_DIR/r2-3po.service" "$UNIT_DEST"
root_run systemctl daemon-reload
cleanup_unit_staging
UNIT_STAGING_DIR=""

log "Starting R2's dedicated Ollama server as user r2..."
if ! root_run systemctl enable --now r2-ollama.service; then
    root_run systemctl --no-pager --full status r2-ollama.service || true
    root_run journalctl -u r2-ollama.service -n 80 --no-pager || true
    fail "R2's private Ollama server could not start. The existing user's Ollama service was not modified."
fi
# Match the exact default policy used by r2_service_runner.sh. A generic
# no-options probe can pass on CPU while the actual 8-layer/adaptive-context
# service configuration fails, so test the real policy before replacing R2.
NVIDIA_PCI_PRESENT=0
for vendor_file in /sys/bus/pci/devices/*/vendor; do
    [[ -r "$vendor_file" ]] || continue
    NVIDIA_VENDOR=""
    IFS= read -r NVIDIA_VENDOR < "$vendor_file" || continue
    NVIDIA_CLASS=""
    IFS= read -r NVIDIA_CLASS < "${vendor_file%/vendor}/class" 2>/dev/null || continue
    if [[ "$NVIDIA_VENDOR" == "0x10de" && "$NVIDIA_CLASS" == 0x03* ]]; then
        NVIDIA_PCI_PRESENT=1
        break
    fi
done
NVIDIA_QUERY_OK=0
NVIDIA_GPU_NAMES=""
if command -v nvidia-smi >/dev/null 2>&1; then
    if NVIDIA_GPU_NAMES="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null)" &&
       [[ -n "$NVIDIA_GPU_NAMES" ]]; then
        NVIDIA_QUERY_OK=1
    fi
fi
if [[ "$NVIDIA_PCI_PRESENT" == "1" && "$NVIDIA_QUERY_OK" != "1" ]]; then
    fail "NVIDIA PCI hardware is present but nvidia-smi cannot query a GPU; refusing to silently deploy a CPU-only policy."
fi

PREFLIGHT_GPU=0
PREFLIGHT_CTX=""
PREFLIGHT_POLICY_DEFAULTED=0
PREFLIGHT_GPU_ARGS=()
if [[ "$NVIDIA_QUERY_OK" == "1" ]] &&
   printf '%s\n' "$NVIDIA_GPU_NAMES" | grep -qi 'GTX 1050 Ti'; then
    PREFLIGHT_GPU=8
    PREFLIGHT_POLICY_DEFAULTED=1
fi
if [[ "$NVIDIA_PCI_PRESENT" == "1" || "$PREFLIGHT_GPU" != "0" ]]; then
    PREFLIGHT_GPU_ARGS=(--require-gpu-offload)
fi
log "Preflight GPU/context policy: R2_OLLAMA_NUM_GPU=$PREFLIGHT_GPU R2_OLLAMA_NUM_CTX=${PREFLIGHT_CTX:-adaptive}"
if ! run_as_r2 env R2_MODEL="$R2_MODEL" R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
    R2_OLLAMA_NUM_GPU="$PREFLIGHT_GPU" R2_OLLAMA_NUM_CTX="$PREFLIGHT_CTX" \
    python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --wait-seconds 60 --timeout 180 "${PREFLIGHT_GPU_ARGS[@]}"; then
    if [[ "$PREFLIGHT_POLICY_DEFAULTED" == "1" ]]; then
        log "Default GTX 1050 Ti preflight failed; retrying with 4 GPU layers and 8192 context tokens."
        PREFLIGHT_GPU=4
        PREFLIGHT_CTX=8192
        if ! run_as_r2 env R2_MODEL="$R2_MODEL" R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
            R2_OLLAMA_NUM_GPU="$PREFLIGHT_GPU" R2_OLLAMA_NUM_CTX="$PREFLIGHT_CTX" \
            python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --wait-seconds 30 --timeout 180 "${PREFLIGHT_GPU_ARGS[@]}"; then
            root_run systemctl stop r2-ollama.service >/dev/null 2>&1 || true
            fail "R2's private Ollama server failed both the 8-layer/adaptive-context and 4-layer/8192-context generation preflights; the previous R2 executable was preserved."
        fi
    else
        root_run systemctl stop r2-ollama.service >/dev/null 2>&1 || true
        fail "R2's private Ollama server did not pass a real $R2_MODEL generation test; the previous R2 executable was preserved."
    fi
fi

# Preserve the last known executable only after the private model is usable.
# Refuse symlinks and unexpected file types rather than backing up an ambiguous target.
if [[ -L "$R2_EXEC" ]]; then
    fail "refusing symlinked existing executable: $R2_EXEC"
fi
if [[ -e "$R2_EXEC" ]]; then
    [[ -f "$R2_EXEC" && -x "$R2_EXEC" ]] ||
        fail "existing executable is not a regular executable file: $R2_EXEC"
    BACKUP_EXEC="$(run_as_r2 mktemp "$R2_ROOT/r2.previous.XXXXXX")" ||
        fail "could not reserve a rollback copy path as r2."
    if ! run_as_r2 cp "$R2_EXEC" "$BACKUP_EXEC"; then
        run_as_r2 rm -f "$BACKUP_EXEC"
        BACKUP_EXEC=""
        fail "could not preserve the previous executable; existing installation was left untouched."
    fi
    run_as_r2 chmod 0755 "$BACKUP_EXEC"
    log "Previous executable preserved at $BACKUP_EXEC"
fi

run_as_r2 mv -f "$TEMP_EXEC" "$R2_EXEC"
NEW_EXEC_INSTALLED=1
run_as_r2 chmod 0755 "$R2_EXEC"

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

INSTALL_SUCCEEDED=1
log "R2 core initialization was observed; service is active and enabled for boot."
root_run systemctl --no-pager --full status r2-3po.service || true
log "Live logs: journalctl -u r2-3po.service -f"
log "Stop cleanly: sudo systemctl stop r2-3po.service"
