#!/usr/bin/env bash
# Long-running, non-interactive R2 service entrypoint.
set -euo pipefail

R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"
R2_SOURCE="${R2_SOURCE:-$R2_ROOT/R2}"
R2_EXEC="${R2_EXEC:-$R2_ROOT/r2}"
R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"
OLLAMA_TAGS_URL="http://127.0.0.1:11434/api/tags"
R2_MODEL="${R2_MODEL:-gemma3:4b}"
R2_VISION_MODEL="$R2_MODEL"
export R2_MODEL R2_VISION_MODEL

# Keep the runtime account's home-relative defaults inside the canonical R2
# project tree; the Linux account's actual home is not a second data directory.
export HOME="$R2_ROOT"
export USER="r2"
export LOGNAME="r2"

log() { printf '[R2 service] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit 1; }

# Match the interactive launcher: prefer partial offload on the target 4 GiB
# GTX 1050 Ti, but keep automatic placement when that card is not installed.
# An explicit service environment override always takes precedence.
if [[ -z "${R2_OLLAMA_NUM_GPU+x}" ]]; then
    if command -v nvidia-smi >/dev/null 2>&1 &&
       nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | grep -qi 'GTX 1050 Ti'; then
        export R2_OLLAMA_NUM_GPU=20
    else
        export R2_OLLAMA_NUM_GPU=0
    fi
fi
log "Ollama GPU layer policy: R2_OLLAMA_NUM_GPU=$R2_OLLAMA_NUM_GPU"

# Acquire the sleep inhibitor before waiting for Ollama or doing any preflight.
# Re-enter this script under the inhibitor with real time; faketime is applied
# only to the final R2 process, never to systemd-inhibit or its checks.
if [[ "${R2_INHIBIT_ACTIVE:-0}" != "1" ]]; then
    if command -v systemd-inhibit >/dev/null 2>&1 &&
       systemd-inhibit --what=sleep --mode=block --who=R2-3PO \
           --why="Check R2-3PO sleep inhibitor support" /usr/bin/true >/dev/null 2>&1; then
        exec systemd-inhibit --what=sleep --mode=block --who=R2-3PO \
            --why="Keep R2-3PO running for its overnight session" \
            /usr/bin/env R2_INHIBIT_ACTIVE=1 "$0" "$@"
    else
        export R2_INHIBIT_UNAVAILABLE=1
    fi
fi

[[ "$(id -un)" == "r2" ]] || fail "must run as the dedicated Linux account 'r2' (systemd unit configures this)."
[[ -x "$R2_EXEC" ]] || fail "executable missing or not executable: $R2_EXEC; run tools/install_r2_overnight.sh first."
[[ -d "$R2_SOURCE" && -r "$R2_SOURCE" && -x "$R2_SOURCE" ]] || fail "source/runtime directory is not readable: $R2_SOURCE"
[[ -w "$R2_ROOT" ]] || fail "R2 runtime root is not writable by r2: $R2_ROOT"
[[ -w "$R2_SOURCE" ]] || fail "R2 source/database directory is not writable by r2: $R2_SOURCE (SQLite and persistent memory need write access)"
[[ -d "$R2_ROOT/R2_Diary" && -w "$R2_ROOT/R2_Diary" ]] || fail "R2 diary directory is missing or not writable by r2: $R2_ROOT/R2_Diary"
for database in "$R2_SOURCE/r2_memory.db" "$R2_SOURCE/r2_reality.db" "$R2_SOURCE/r2_fridge.db" "$R2_SOURCE/r2_rewards.db" "$R2_SOURCE/r2_addictions.db"; do
    [[ ! -L "$database" ]] || fail "refusing symlinked R2 database: $database"
    if [[ -e "$database" ]]; then
        [[ -f "$database" && -r "$database" && -w "$database" ]] || fail "existing database is not a readable/writable regular file for r2: $database"
    fi
done
command -v curl >/dev/null 2>&1 || fail "curl is required to check the local Ollama service."
command -v python3 >/dev/null 2>&1 || fail "python3 is required for the local Ollama model preflight."
command -v date >/dev/null 2>&1 || fail "date is required for R2's private clock."

# Missing sensory/media helpers should be visible in the journal instead of
# silently making a subsystem appear operational when its runtime is absent.
for helper in ffmpeg ffprobe pactl xdotool; do
    if ! command -v "$helper" >/dev/null 2>&1; then
        log "WARNING: $helper is unavailable; related Eyes/Ears/VLC/controller features may be degraded."
    fi
done
if ! command -v mpg123 >/dev/null 2>&1 &&
   ! command -v ffplay >/dev/null 2>&1 &&
   ! command -v mpv >/dev/null 2>&1; then
    log "WARNING: no local MP3 player (mpg123, ffplay, or mpv) is installed; beep/whistle playback cannot work."
fi
if ! command -v vlc >/dev/null 2>&1 ||
   ! python3 -c 'import vlc' >/dev/null 2>&1; then
    log "WARNING: VLC or python3-vlc is unavailable; the CRT VCR GUI cannot render tapes."
fi
if ! python3 -c 'import tkinter' >/dev/null 2>&1; then
    log "WARNING: Tkinter is unavailable; the TV and Observer desktop windows cannot open."
fi

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
[[ -n "$FAKETIME_LIB" && -r "$FAKETIME_LIB" ]] || fail "libfaketime.so.1 not found. Install libfaketime outside R2's virtual-time environment; do not update package indexes from this launcher."

# Wait for the local model and verify one actual generation before entering the core.
# This is localhost-only; no model downloads or external network requests occur.
[[ -r "$R2_SOURCE/tools/preflight_ollama_model.py" ]] ||
    fail "shared Ollama inference preflight is missing: $R2_SOURCE/tools/preflight_ollama_model.py"
log "Waiting for local $R2_MODEL availability and a real chat completion..."
if ! R2_MODEL="$R2_MODEL" python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --wait-seconds 60 --timeout 180; then
    fail "Ollama did not complete a real $R2_MODEL chat preflight. Check the local model/server; systemd will retry automatically."
fi

# Preserve the established private, continuously advancing 1999 clock. The host clock is untouched.
# Capture the host epoch once so a midnight boundary cannot mix two dates.
REAL_EPOCH="$(date '+%s')"
REAL_NOW="$(date -d "@$REAL_EPOCH" '+%Y-%m-%d %H:%M:%S')"
CURRENT_MONTH="$(date -d "@$REAL_EPOCH" '+%m')"
CURRENT_DAY="$(date -d "@$REAL_EPOCH" '+%d')"
CURRENT_HOUR="$(date -d "@$REAL_EPOCH" '+%H')"
CURRENT_MINUTE="$(date -d "@$REAL_EPOCH" '+%M')"
CURRENT_SECOND="$(date -d "@$REAL_EPOCH" '+%S')"
R2_START="1999-${CURRENT_MONTH}-${CURRENT_DAY} ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"
if ! R2_EPOCH="$(date -d "$R2_START" '+%s' 2>/dev/null)"; then
    # 1999 was not a leap year. Map Feb 29 to Mar 1 instead of failing startup.
    if [[ "$CURRENT_MONTH$CURRENT_DAY" == "0229" ]]; then
        R2_START="1999-03-01 ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"
        R2_EPOCH="$(date -d "$R2_START" '+%s')" || fail "could not calculate R2's private clock."
    else
        fail "could not calculate R2's private clock."
    fi
fi
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
    "HOME=$R2_ROOT"
    "USER=r2"
    "LOGNAME=r2"
    "LD_PRELOAD=$FAKETIME_LIB"
    "FAKETIME=$FAKETIME_OFFSET"
    "FAKETIME_DONT_RESET=1"
    "R2_SERVICE_MODE=1"
    "R2_OLLAMA_TIMEOUT_SECONDS=${R2_OLLAMA_TIMEOUT_SECONDS:-}"
    "R2_OLLAMA_NUM_GPU=$R2_OLLAMA_NUM_GPU"
    "R2_FX_DIR=$R2_FX_DIR"
    "R2_MODEL=$R2_MODEL"
    "R2_VISION_MODEL=$R2_VISION_MODEL"
    "$R2_EXEC")

# The inhibitor, when available, already wraps this entire runner and its child.
# The service still shuts down normally on systemctl stop or SIGTERM.
if [[ "${R2_INHIBIT_UNAVAILABLE:-0}" == "1" ]]; then
    log "WARNING: system sleep inhibition is unavailable; host suspend may pause the overnight session."
fi
exec "${R2_ENV[@]}"
