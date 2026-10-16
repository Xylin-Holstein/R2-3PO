#!/usr/bin/env bash
# Run Ollama's HTTP server as the dedicated R2 account, never as the desktop user.
set -euo pipefail

[[ "$(id -un)" == "r2" ]] || {
    echo "[R2 Ollama] ERROR: this server must run as Linux user r2." >&2
    exit 1
}
export HOME="${HOME:-/home/x/R2_Home}"
export USER=r2 LOGNAME=r2
export OLLAMA_HOST="${OLLAMA_HOST:-127.0.0.1:11435}"
export OLLAMA_MODELS="${OLLAMA_MODELS:-$HOME/.ollama/models}"

case "$OLLAMA_HOST" in
    127.0.0.1:11435) ;;
    *) echo "[R2 Ollama] ERROR: refusing non-R2-local OLLAMA_HOST=$OLLAMA_HOST" >&2; exit 1 ;;
esac

mkdir -p "$OLLAMA_MODELS"
OLLAMA_BIN="$(command -v ollama || true)"
[[ -n "$OLLAMA_BIN" && -x "$OLLAMA_BIN" ]] || {
    echo "[R2 Ollama] ERROR: the ollama executable is missing from PATH." >&2
    exit 1
}
echo "[R2 Ollama] Serving as $(id -un) on $OLLAMA_HOST using $OLLAMA_MODELS"
exec "$OLLAMA_BIN" serve
