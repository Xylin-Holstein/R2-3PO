#!/bin/bash

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"

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

    "$R2_SOURCE/Visual.c"
    "$R2_SOURCE/Visual.h"

    "$R2_SOURCE/Remote.c"
    "$R2_SOURCE/Remote.h"

    "$R2_SOURCE/Ears.c"
    "$R2_SOURCE/Ears.h"

    "$R2_SOURCE/Eyes.c"
    "$R2_SOURCE/Eyes.h"

    "$R2_SOURCE/shell.c"
    "$R2_SOURCE/shell.h"
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

gcc -std=c11 -Wall -Wextra -O2 \
    "$R2_SOURCE/r2.c" \
    "$R2_SOURCE/shell.c" \
    "$R2_SOURCE/r2_diary.c" \
    "$R2_SOURCE/Log.c" \
    "$R2_SOURCE/Visual.c" \
    "$R2_SOURCE/Remote.c" \
    "$R2_SOURCE/Ears.c" \
    "$R2_SOURCE/Eyes.c" \
    -o "$R2_EXEC" \
    -lcurl \
    -lsqlite3 \
    -lpthread \
    -ljson-c \
    -lpulse-simple \
    -lpulse \
    -lm

status=$?

if [ $status -ne 0 ]; then
    echo
    echo "========================================"
    echo "ERROR: R2 compilation failed."
    echo "R2 was NOT launched."
    echo "========================================"
    echo
    read -p "Press Enter to exit..."
    exit $status
fi

echo
echo "Compilation successful."
echo "Executable updated:"
echo "    $R2_EXEC"
echo
echo "Default vision model: qwen2.5vl:3b"
echo "Remote API: disabled unless R2_REMOTE_TOKEN is configured (24+ characters)."
echo "For private remote access, use Tailscale and do not port-forward port 8765."
echo "R2's conversation model remains llama3; vision perception is handled by the separate local model."
echo "If it is not installed, run: ollama pull qwen2.5vl:3b"
echo
echo "Modules compiled:"
echo "    r2.c"
echo "    shell.c"
echo "    r2_diary.c"
echo "    Log.c"
echo "    Visual.c"
echo "    Remote.c"
echo "    Ears.c"
echo "    Eyes.c"
echo

echo "[4/4] Transitioning to Linux user: r2..."
echo

echo "Launching R2..."
echo

# Create a writable directory for the human-readable daily Life Log.
# R2 runs as the dedicated r2 account, so create it before dropping privileges.
if ! sudo install -d -o r2 -m 0750 "$R2_ROOT/R2_Log"; then
    echo "WARNING: Could not prepare $R2_ROOT/R2_Log; daily text logging may fail."
fi

# ------------------------------------------------------------
# START R2 WITH ITS PRIVATE CLOCK
# ------------------------------------------------------------

sudo -u r2 --preserve-env=R2_REMOTE_TOKEN,R2_REMOTE_PORT \
    env \
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
