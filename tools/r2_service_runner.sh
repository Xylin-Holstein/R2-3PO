#!/usr/bin/env bash
# Long-running, non-interactive R2 service entrypoint.
set -euo pipefail

R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"
R2_SOURCE="${R2_SOURCE:-$R2_ROOT/R2}"
R2_EXEC="${R2_EXEC:-$R2_ROOT/r2}"
R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"
OLLAMA_TAGS_URL="http://127.0.0.1:11434/api/tags"

log() { printf '[R2 service] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit 1; }

[[ "$(id -un)" == "r2" ]] || fail "must run as the dedicated Linux account 'r2' (systemd unit configures this)."
[[ -x "$R2_EXEC" ]] || fail "executable missing or not executable: $R2_EXEC; run tools/install_r2_overnight.sh first."
[[ -d "$R2_SOURCE" && -r "$R2_SOURCE" && -x "$R2_SOURCE" ]] || fail "source/runtime directory is not readable: $R2_SOURCE"
[[ -w "$R2_ROOT" ]] || fail "R2 runtime root is not writable by r2: $R2_ROOT"
[[ -w "$R2_SOURCE" ]] || fail "R2 source/database directory is not writable by r2: $R2_SOURCE (SQLite and persistent memory need write access)"
[[ -d "$R2_ROOT/R2_Diary" && -w "$R2_ROOT/R2_Diary" ]] || fail "R2 diary directory is missing or not writable by r2: $R2_ROOT/R2_Diary"
command -v curl >/dev/null 2>&1 || fail "curl is required to check the local Ollama service."
command -v python3 >/dev/null 2>&1 || fail "python3 is required for the local Ollama model preflight."
command -v date >/dev/null 2>&1 || fail "date is required for R2's private clock."

FAKETIME_LIB="${R2_FAKETIME_LIB:-}"
if [[ -z "$FAKETIME_LIB" ]]; then
    for candidate in \
        /usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1 \
        /usr/lib/x86_64-linux-gnu/libfaketime.so.1 \
        /usr/lib/faketime/libfaketime.so.1 \
        /usr/local/lib/faketime/libfaketime.so.1 \
        /usr/local/lib/libfaketime.so.1; do
        if [[ -r "$candidate" ]]; then FAKETIME_LIB="$candidate"; break; fi
    done
fi
[[ -n "$FAKETIME_LIB" && -r "$FAKETIME_LIB" ]] || fail "libfaketime.so.1 not found. Install libfaketime outside R2's virtual-time environment; do not run apt-get update from this launcher."

# Wait briefly for Ollama after boot/restart. Do not download models or contact the internet.
log "Waiting for the local Ollama API..."
OLLAMA_READY=0
for ((attempt=1; attempt<=30; attempt++)); do
    if curl --silent --show-error --fail --max-time 2 "$OLLAMA_TAGS_URL" 2>/dev/null |
        python3 -c 'import json,sys; d=json.load(sys.stdin); sys.exit(0 if any(x.get("name")=="gemma3:4b" or x.get("name","").startswith("gemma3:4b-") for x in d.get("models",[])) else 3)' >/dev/null 2>&1; then
        OLLAMA_READY=1
        break
    fi
    sleep 2
done
[[ "$OLLAMA_READY" -eq 1 ]] || fail "Ollama did not become ready with gemma3:4b after 60 seconds. Start Ollama and make sure 'ollama list' contains gemma3:4b; service will retry automatically."

# Preserve the established private, continuously advancing 1999 clock. The host clock is untouched.
REAL_NOW="$(date '+%Y-%m-%d %H:%M:%S')"
CURRENT_MONTH="$(date '+%m')"
CURRENT_DAY="$(date '+%d')"
CURRENT_HOUR="$(date '+%H')"
CURRENT_MINUTE="$(date '+%M')"
CURRENT_SECOND="$(date '+%S')"
R2_START="1999-${CURRENT_MONTH}-${CURRENT_DAY} ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"
REAL_EPOCH="$(date -d "$REAL_NOW" '+%s')"
R2_EPOCH="$(date -d "$R2_START" '+%s')" || fail "could not calculate R2's private clock."
[[ "$REAL_EPOCH" =~ ^-?[0-9]+$ && "$R2_EPOCH" =~ ^-?[0-9]+$ ]] || fail "could not calculate R2's private clock."
FAKETIME_OFFSET=$((R2_EPOCH - REAL_EPOCH))

log "Starting R2 with private clock $R2_START; host system time will not change."
log "Sound assets: $R2_FX_DIR"
SOUND_AUDIT="$R2_SOURCE/tools/audit_r2_sound_assets.py"
if [[ -f "$SOUND_AUDIT" ]]; then
    python3 "$SOUND_AUDIT" --root "$R2_FX_DIR" || log "WARNING: sound inventory failed; R2 core will still start without guaranteed beep/whistle playback."
else
    log "WARNING: sound asset audit script is missing: $SOUND_AUDIT"
fi

# Keep the host clock and service-manager tools on real time. Apply faketime only
# to R2 itself; otherwise systemd-inhibit would inherit the private 1999 clock.
R2_ENV=(/usr/bin/env
    "LD_PRELOAD=$FAKETIME_LIB"
    "FAKETIME=$FAKETIME_OFFSET"
    "FAKETIME_DONT_RESET=1"
    "R2_SERVICE_MODE=1"
    "R2_FX_DIR=$R2_FX_DIR"
    "R2_VISION_MODEL=gemma3:4b"
    "$R2_EXEC")

# systemd-inhibit blocks suspend while R2 is alive, so an overnight session can continue.
# The service still shuts down normally on systemctl stop or SIGTERM.
if command -v systemd-inhibit >/dev/null 2>&1; then
    exec systemd-inhibit --what=sleep --mode=block --who=R2-3PO --why="Keep R2-3PO running for its overnight session" "${R2_ENV[@]}"
else
    log "WARNING: systemd-inhibit unavailable; host suspend may pause the overnight session."
    exec "${R2_ENV[@]}"
fi
