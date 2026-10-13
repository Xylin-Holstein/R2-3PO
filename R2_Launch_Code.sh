#!/bin/bash

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
R2_MODEL="${R2_MODEL:-gemma3:4b}"
R2_VISION_MODEL="$R2_MODEL"
R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11435}"
if [ "$R2_OLLAMA_BASE_URL" != "http://127.0.0.1:11435" ]; then
    echo "ERROR: R2 must use its dedicated r2-owned Ollama endpoint at http://127.0.0.1:11435."
    exit 1
fi

echo "========================================"
echo "        R2-3PO LAUNCH SEQUENCE"
echo "========================================"
echo

cd "$R2_ROOT" || {
    echo "ERROR: Could not enter R2 workspace."
    read -p "Press Enter to exit..."
    exit 1
}

echo "[1/4] Checking R2 source modules..."
echo

REQUIRED_FILES=(
    "$R2_SOURCE/r2.c"
    "$R2_SOURCE/r2.h"

    "$R2_SOURCE/r2_diary.c"
    "$R2_SOURCE/r2_diary.h"

    "$R2_SOURCE/Log.c"
    "$R2_SOURCE/Log.h"

    "$R2_SOURCE/Reality.c"
    "$R2_SOURCE/Reality.h"
    "$R2_SOURCE/Addiction.c"
    "$R2_SOURCE/Addiction.h"
    "$R2_SOURCE/Reward.c"
    "$R2_SOURCE/Reward.h"
    "$R2_SOURCE/AlternateSelf.c"
    "$R2_SOURCE/AlternateSelf.h"
    "$R2_SOURCE/R2Reply.c"
    "$R2_SOURCE/R2Reply.h"
    "$R2_SOURCE/R2Sounds.c"
    "$R2_SOURCE/R2Sounds.h"

    "$R2_SOURCE/Visual.c"
    "$R2_SOURCE/Visual.h"

    "$R2_SOURCE/Ears.c"
    "$R2_SOURCE/Ears.h"

    "$R2_SOURCE/Eyes.c"
    "$R2_SOURCE/Eyes.h"

    "$R2_SOURCE/shell.c"
    "$R2_SOURCE/shell.h"

    "$R2_SOURCE/TV.py"
    "$R2_SOURCE/GameBoyAdvance.py"
)

for FILE in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$FILE" ]; then
        echo "ERROR: Missing R2 source file:"
        echo "       $FILE"
        echo
        echo "R2 was NOT launched."
        read -p "Press Enter to exit..."
        exit 1
    fi
done

echo "All R2 source modules found."
echo

R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"
if [ -f "$R2_PREFLIGHT" ]; then
    echo "[Preflight] Checking R2 runtime, databases, model, and device permissions..."
    if ! bash "$R2_PREFLIGHT"; then
        echo
        echo "R2 launch cancelled: preflight reported one or more failures."
        read -r -p "Press Enter to continue launching R2, or Ctrl+C to cancel..."
    fi
else
    echo "ERROR: Missing read-only R2 preflight: $R2_PREFLIGHT"
    exit 1
fi

if sudo systemctl is-active --quiet r2-3po.service 2>/dev/null; then
    echo "ERROR: The overnight R2 core is already running as a service."
    echo "Stop it before launching an interactive core: sudo systemctl stop r2-3po.service"
    exit 1
fi
if pgrep -x r2 >/dev/null 2>&1; then
    echo "ERROR: Another R2 core is already running; refusing to open a duplicate."
    exit 1
fi

echo "[2/4] Configuring R2 private clock..."
echo

FAKETIME_LIB=""

FAKETIME_PATHS=(
    "/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1"
    "/usr/lib/x86_64-linux-gnu/libfaketime.so.1"
    "/usr/lib/faketime/libfaketime.so.1"
    "/usr/local/lib/faketime/libfaketime.so.1"
    "/usr/local/lib/libfaketime.so.1"
)

# ------------------------------------------------------------
# FIND LIBFAKETIME
# ------------------------------------------------------------

for PATH_CHECK in "${FAKETIME_PATHS[@]}"; do
    if [ -f "$PATH_CHECK" ]; then
        FAKETIME_LIB="$PATH_CHECK"
        break
    fi
done

# ------------------------------------------------------------
# AUTOMATICALLY INSTALL LIBFAKETIME IF MISSING
#
# IMPORTANT:
# We intentionally DO NOT run "apt-get update".
#
# R2's virtual clock is deliberately set in the past.
# Package-management operations must therefore happen outside
# R2's virtual-time environment.
#
# apt uses whatever package metadata is already installed on
# the host system.
# ------------------------------------------------------------

if [ -z "$FAKETIME_LIB" ]; then
    echo "libfaketime was not found."
    echo
    echo "Installing libfaketime from the existing package cache..."
    echo

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "ERROR: apt-get is not available."
        echo "Cannot automatically install libfaketime."
        echo
        echo "R2 was NOT launched."
        read -p "Press Enter to exit..."
        exit 1
    fi

    sudo apt-get install -y libfaketime

    status=$?

    if [ $status -ne 0 ]; then
        echo
        echo "========================================"
        echo "ERROR: libfaketime installation failed."
        echo "========================================"
        echo
        echo "The existing package information may not"
        echo "contain libfaketime."
        echo
        echo "R2 was NOT launched."
        read -p "Press Enter to exit..."
        exit $status
    fi

    # Search again after installation.
    FAKETIME_LIB=""

    for PATH_CHECK in "${FAKETIME_PATHS[@]}"; do
        if [ -f "$PATH_CHECK" ]; then
            FAKETIME_LIB="$PATH_CHECK"
            break
        fi
    done
fi

# ------------------------------------------------------------
# VERIFY INSTALLATION
# ------------------------------------------------------------

if [ -z "$FAKETIME_LIB" ]; then
    echo
    echo "========================================"
    echo "ERROR: libfaketime could not be located."
    echo "========================================"
    echo
    echo "The package installation completed, but"
    echo "libfaketime.so.1 could not be found."
    echo
    echo "R2 was NOT launched."
    read -p "Press Enter to exit..."
    exit 1
fi

echo "libfaketime ready:"
echo "    $FAKETIME_LIB"
echo

# ------------------------------------------------------------
# R2 PRIVATE 1999 CLOCK
#
# We do NOT modify Linux's actual system clock.
#
# We calculate the exact timestamp corresponding to the
# current real date/time with the YEAR changed to 1999.
#
# The resulting difference is supplied to libfaketime as a
# timestamp offset.
#
# Because this is an offset from the real running clock,
# time continues moving normally.
#
# Therefore:
#
#   1999-12-31 23:59:59
#              ↓
#   2000-01-01 00:00:00
#
# happens naturally.
#
# The user's normal Linux environment is untouched.
# ------------------------------------------------------------

REAL_NOW=$(date '+%Y-%m-%d %H:%M:%S')

CURRENT_MONTH=$(date '+%m')
CURRENT_DAY=$(date '+%d')
CURRENT_HOUR=$(date '+%H')
CURRENT_MINUTE=$(date '+%M')
CURRENT_SECOND=$(date '+%S')

R2_START="1999-${CURRENT_MONTH}-${CURRENT_DAY} ${CURRENT_HOUR}:${CURRENT_MINUTE}:${CURRENT_SECOND}"

REAL_EPOCH=$(date -d "$REAL_NOW" '+%s')
R2_EPOCH=$(date -d "$R2_START" '+%s')

if [ -z "$REAL_EPOCH" ] || [ -z "$R2_EPOCH" ]; then
    echo "ERROR: Could not calculate R2 virtual time."
    echo
    echo "R2 was NOT launched."
    read -p "Press Enter to exit..."
    exit 1
fi

FAKETIME_OFFSET=$((R2_EPOCH - REAL_EPOCH))

echo "Real system time:"
echo "    $REAL_NOW"
echo

echo "R2 virtual time:"
echo "    $R2_START"
echo

echo "R2 clock offset:"
echo "    ${FAKETIME_OFFSET} seconds"
echo

echo "R2 clock mode:"
echo "    PRIVATE / CONTINUOUS"
echo
echo "The Linux system clock will NOT be changed."
echo "R2 will begin in 1999 and continue normally into 2000."
echo

echo "[3/4] Recompiling R2..."
echo

TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"
if [ -e "$TEMP_EXEC" ] || [ -L "$TEMP_EXEC" ]; then
    echo "ERROR: Temporary executable path already exists: $TEMP_EXEC"
    exit 1
fi

gcc -std=c11 -Wall -Wextra -Werror -O2 \
    "$R2_SOURCE/r2.c" \
    "$R2_SOURCE/shell.c" \
    "$R2_SOURCE/r2_diary.c" \
    "$R2_SOURCE/Log.c" \
    "$R2_SOURCE/Reality.c" \
    "$R2_SOURCE/Addiction.c" \
    "$R2_SOURCE/Reward.c" \
    "$R2_SOURCE/AlternateSelf.c" \
    "$R2_SOURCE/R2Reply.c" \
    "$R2_SOURCE/R2Sounds.c" \
    "$R2_SOURCE/Visual.c" \
    "$R2_SOURCE/Ears.c" \
    "$R2_SOURCE/Eyes.c" \
    -o "$TEMP_EXEC" \
    -lcurl \
    -lsqlite3 \
    -lpthread \
    -lpthread \
    -ljson-c \
    -lpulse-simple \
    -lpulse \
    -lm

status=$?

if [ $status -ne 0 ]; then
    rm -f "$TEMP_EXEC"
    echo
    echo "========================================"
    echo "ERROR: R2 compilation failed."
    echo "The previous executable was preserved; R2 was NOT launched."
    echo "========================================"
    echo
    read -p "Press Enter to exit..."
    exit $status
fi

if ! chmod 0755 "$TEMP_EXEC" || ! mv -f "$TEMP_EXEC" "$R2_EXEC"; then
    rm -f "$TEMP_EXEC"
    echo "ERROR: Could not atomically install the new executable; the previous executable was preserved."
    exit 1
fi

echo
echo "Compilation successful."
echo "Executable updated atomically:"
echo "    $R2_EXEC"
echo
echo "Unified conversation and vision model: $R2_MODEL"
echo "R2 uses the dedicated r2-owned Ollama service on 127.0.0.1:11435."
echo
echo "Modules compiled:"
echo "    r2.c"
echo "    shell.c"
echo "    r2_diary.c"
echo "    Log.c"
echo "    Reality.c"
echo "    Addiction.c"
echo "    Reward.c"
echo "    AlternateSelf.c"
echo "    R2Reply.c"
echo "    R2Sounds.c"
echo "    Visual.c"
echo "    Ears.c"
echo "    Eyes.c"
echo

echo "[4/4] Transitioning to Linux user: r2..."
echo

echo "Launching R2..."
echo

# The private inference service owns its UID-scoped IPv4/IPv6 network policy.
# Never fall back to the desktop user's shared Ollama service.
if ! sudo systemctl is-active --quiet r2-ollama.service; then
    echo "[Ollama] Starting the dedicated r2-owned model service..."
    if ! sudo systemctl start r2-ollama.service; then
        echo "ERROR: r2-ollama.service is not installed or could not start."
        echo "Install it with: sudo bash $R2_SOURCE/tools/install_r2_overnight.sh"
        exit 1
    fi
fi
OLLAMA_SERVICE_USER="$(sudo systemctl show r2-ollama.service -p User --value 2>/dev/null | tr -d '[:space:]')"
if [ "$OLLAMA_SERVICE_USER" != "r2" ]; then
    echo "ERROR: r2-ollama.service must run as Linux user r2; found '$OLLAMA_SERVICE_USER'."
    exit 1
fi
if ! sudo -u r2 env R2_MODEL="$R2_MODEL" R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" python3 -c '
import json, os, urllib.request
base = os.environ["R2_OLLAMA_BASE_URL"].rstrip("/")
model = os.environ["R2_MODEL"]
with urllib.request.urlopen(base + "/api/tags", timeout=15) as response:
    data = json.load(response)
names = {entry.get("name", "") for entry in data.get("models", [])}
if not any(name == model or name.startswith(model + "-") for name in names):
    raise SystemExit("required R2 model is missing from the private Ollama store: " + model)
' ; then
    echo "ERROR: R2's private Ollama endpoint is not ready or $R2_MODEL is not installed."
    echo "No fallback to the desktop user's Ollama server will be attempted."
    exit 1
fi

if ! sudo -u r2 env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 \
    R2_MODEL="$R2_MODEL" R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
    python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --wait-seconds 60 --timeout 180; then
    echo "ERROR: Gemma is listed but did not complete a real chat generation through R2's private endpoint."
    echo "R2 will not launch with a disconnected or non-generative model."
    exit 1
fi

# ------------------------------------------------------------
# START R2 WITH ITS PRIVATE CLOCK
# ------------------------------------------------------------

sudo -u r2 \
    env \
    HOME="$R2_ROOT" USER=r2 LOGNAME=r2 \
    R2_MODEL="$R2_MODEL" \
    R2_VISION_MODEL="$R2_VISION_MODEL" \
    R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
    DISPLAY="${DISPLAY:-}" \
    XAUTHORITY="${XAUTHORITY:-}" \
    LD_PRELOAD="$FAKETIME_LIB" \
    FAKETIME="${FAKETIME_OFFSET}" \
    FAKETIME_DONT_RESET=1 \
    R2_VISION_MODEL="${R2_VISION_MODEL:-qwen2.5vl:3b}" \
    "$R2_EXEC"

status=$?

echo
echo "========================================"
echo "R2 exited with status: $status"
echo "========================================"
echo

read -p "Press Enter to close..."
exit $status
