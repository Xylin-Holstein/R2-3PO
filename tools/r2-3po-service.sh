#!/usr/bin/env bash
set -Eeuo pipefail

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
MODEL="gemma3:4b"
R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"

log() { printf '[R2 service] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit 1; }

[[ "$(id -un)" == "r2" ]] || fail "Must run as the dedicated Linux account r2."
[[ -x "$R2_EXEC" ]] || fail "Executable missing: $R2_EXEC. Run tools/install_r2_service.sh."
[[ -r "$R2_SOURCE/r2.c" ]] || fail "R2 source is not readable: $R2_SOURCE/r2.c"
[[ -r "$R2_FX_DIR" && -d "$R2_FX_DIR" ]] || log "Sound directory unavailable: $R2_FX_DIR (conversation can still run)."

FAKETIME_LIB=""
for candidate in \
    /usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1 \
    /usr/lib/x86_64-linux-gnu/libfaketime.so.1 \
    /usr/lib/faketime/libfaketime.so.1 \
    /usr/local/lib/faketime/libfaketime.so.1 \
    /usr/local/lib/libfaketime.so.1; do
    if [[ -f "$candidate" ]]; then FAKETIME_LIB="$candidate"; break; fi
done
[[ -n "$FAKETIME_LIB" ]] || fail "libfaketime is missing. Install the Debian/Ubuntu package libfaketime."

command -v curl >/dev/null 2>&1 || fail "curl is required to check the local Ollama server."
command -v python3 >/dev/null 2>&1 || fail "python3 is required to check the local Ollama model."

# Wait briefly for Ollama after boot. The model itself must already be installed.
log "Waiting for the local Ollama API..."
ollama_ready=0
for attempt in $(seq 1 60); do
    if curl -fsS --max-time 3 http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
        ollama_ready=1
        break
    fi
    sleep 2
done
[[ "$ollama_ready" -eq 1 ]] || fail "Ollama API did not become ready within 120 seconds. Check: systemctl status ollama"

if ! curl -fsS --max-time 5 http://127.0.0.1:11434/api/tags | python3 -c '
import json, sys
wanted = "gemma3:4b"
try:
    models = json.load(sys.stdin).get("models", [])
except Exception as exc:
    print("Could not parse Ollama model inventory:", exc, file=sys.stderr)
    sys.exit(2)
names = [item.get("name", "") for item in models]
if not any(name == wanted or name.startswith(wanted + ":") for name in names):
    print("Required local model is missing: " + wanted, file=sys.stderr)
    print("Install it with: ollama pull " + wanted, file=sys.stderr)
    print("Available models: " + (", ".join(names) if names else "(none)"), file=sys.stderr)
    sys.exit(1)
'; then
    fail "The required local model is unavailable; see the model-check message above."
fi

REAL_NOW="$(date '+%Y-%m-%d %H:%M:%S')"
CURRENT_MONTH="$(date '+%m')"
CURRENT_DAY="$(date '+%d')"
CURRENT_HOUR="$(date '+%H')"
CURRENT_MINUTE="$(date '+%M')"
CURRENT_SECOND="$(date '+%S')"
R2_START="1999-${CURRENT_MONTH}-${CURRENT_DAY} ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"
REAL_EPOCH="$(date -d "$REAL_NOW" '+%s')"
if ! R2_EPOCH="$(date -d "$R2_START" '+%s' 2>/dev/null)"; then
    # 1999 was not a leap year; map Feb 29 to Mar 1 instead of failing startup.
    if [[ "$CURRENT_MONTH$CURRENT_DAY" == "0229" ]]; then
        R2_START="1999-03-01 ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"
        R2_EPOCH="$(date -d "$R2_START" '+%s')" || fail "Could not calculate R2's private clock."
    else
        fail "Could not calculate R2's private clock."
    fi
fi
[[ "$REAL_EPOCH" =~ ^-?[0-9]+$ && "$R2_EPOCH" =~ ^-?[0-9]+$ ]] || fail "Invalid timestamp while calculating R2's private clock."
FAKETIME_OFFSET=$((R2_EPOCH - REAL_EPOCH))

export HOME="/home/r2"
export R2_SERVICE_MODE=1
export R2_VISION_MODEL="$MODEL"
export R2_FX_DIR
export LD_PRELOAD="$FAKETIME_LIB"
export FAKETIME="$FAKETIME_OFFSET"
export FAKETIME_DONT_RESET=1
unset DISPLAY XAUTHORITY

log "Starting R2 in headless overnight mode."
log "Virtual clock starts at $R2_START; the host system clock is unchanged."
log "FX inventory path: $R2_FX_DIR"
cd "$R2_ROOT" || fail "Could not enter $R2_ROOT."
exec "$R2_EXEC"
