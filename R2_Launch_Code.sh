#!/bin/bash

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"

echo "========================================"
echo "        R2-3PO LAUNCH SEQUENCE"
echo "========================================"
echo

if ! mkdir -p "$R2_ROOT"; then
    echo "ERROR: Could not create R2 workspace root: $R2_ROOT"
    read -p "Press Enter to exit..."
    exit 1
fi

cd "$R2_ROOT" || {
    echo "ERROR: Could not enter R2 workspace."
    read -p "Press Enter to exit..."
    exit 1
}

# Create only the established workspace directories if they are missing.
# Reality.c uses these same paths; never create parallel lowercase pockets/fridge trees.
echo "[0/4] Checking R2 workspace directories..."
R2_DIRS=(
    "$R2_ROOT/R2"
    "$R2_ROOT/R2_Diary"
    "$R2_ROOT/room"
    "$R2_ROOT/room/shelf"
    "$R2_ROOT/room/box"
    "$R2_ROOT/room/piggybank"
    "$R2_ROOT/Pockets"
    "$R2_ROOT/Pockets/Wallet"
    "$R2_ROOT/fridge"
)
for DIR in "${R2_DIRS[@]}"; do
    if ! mkdir -p "$DIR"; then
        echo "ERROR: Could not create workspace directory: $DIR"
        read -p "Press Enter to exit..."
        exit 1
    fi
done
echo "Workspace directories are ready."
echo

# TV.py and install_tv.sh should be copied into R2/ with the other downloaded
# source files. Install if missing, or refresh the installed GUI if the source
# has changed. The installer creates room/TV/ and VCR_Tapes/ at their canonical paths.
TV_SOURCE="$R2_SOURCE/TV.py"
TV_INSTALLER="$R2_SOURCE/install_tv.sh"
TV_DIR="$R2_ROOT/room/TV"
if [[ -f "$TV_SOURCE" && -f "$TV_INSTALLER" ]]; then
    if [[ ! -x "$TV_DIR/run_tv.sh" || ! -f "$TV_DIR/TV.desktop" ||
          ! -f "$TV_DIR/TV.py" ]] || ! cmp -s "$TV_SOURCE" "$TV_DIR/TV.py"; then
        echo "Installing/updating R2's TV device..."
        if ! bash "$TV_INSTALLER"; then
            echo "WARNING: TV installer failed. R2 will continue startup; TV needs attention."
        fi
    else
        echo "TV device is already installed and up to date."
    fi
else
    echo "TV install skipped: copy TV.py and install_tv.sh into $R2_SOURCE to enable automatic installation."
fi
echo

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

    "$R2_SOURCE/Visual.c"
    "$R2_SOURCE/Visual.h"

    "$R2_SOURCE/Ears.c"
    "$R2_SOURCE/Ears.h"

    "$R2_SOURCE/Eyes.c"
    "$R2_SOURCE/Eyes.h"

    "$R2_SOURCE/R2Sounds.c"
    "$R2_SOURCE/R2Sounds.h"

    "$R2_SOURCE/AlternateSelf.c"
    "$R2_SOURCE/AlternateSelf.h"

    "$R2_SOURCE/Observer.py"

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
    "$R2_SOURCE/Reality.c" \
    "$R2_SOURCE/Addiction.c" \
    "$R2_SOURCE/Reward.c" \
    "$R2_SOURCE/AlternateSelf.c" \
    "$R2_SOURCE/Visual.c" \
    "$R2_SOURCE/Ears.c" \
    "$R2_SOURCE/Eyes.c" \
    "$R2_SOURCE/R2Sounds.c" \
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
echo "Unified conversation + vision model: gemma3:4b"
echo "R2 uses one shared Ollama model for text and image perception."
echo "If needed, install it with: ollama pull gemma3:4b"
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
echo "    Visual.c"
echo "    Ears.c"
echo "    Eyes.c"
echo "    R2Sounds.c"
echo

echo "[4/4] Transitioning to Linux user: r2..."
echo

echo "Launching the read-only observer window..."
echo

# The observer must run as r2 to read the private Life Log database, but
# the desktop X server normally rejects that separate Linux user. Grant only
# r2 access to this local X server, then revoke it when R2 exits.
OBSERVER_XHOST_GRANTED=0
OBSERVER_PID=""
if [ -n "${DISPLAY:-}" ] && command -v xhost >/dev/null 2>&1 && xhost +SI:localuser:r2 >/dev/null 2>&1; then
    OBSERVER_XHOST_GRANTED=1
    # Don't pass the desktop user's cookie or R2's virtual-clock environment.
    sudo -u r2 env -u XAUTHORITY DISPLAY="${DISPLAY}" \
        python3 "$R2_SOURCE/Observer.py" &
    OBSERVER_PID=$!
else
    echo "WARNING: Could not authorize r2 for the desktop display; skipping Observer window."
    echo "R2 will still launch normally."
fi

echo "Launching R2..."
echo

# ------------------------------------------------------------
# START R2 WITH ITS PRIVATE CLOCK
# ------------------------------------------------------------

sudo -u r2 \
    env \
    DISPLAY="${DISPLAY:-}" \
    XAUTHORITY="${XAUTHORITY:-}" \
    LD_PRELOAD="$FAKETIME_LIB" \
    FAKETIME="${FAKETIME_OFFSET}" \
    FAKETIME_DONT_RESET=1 \
    R2_VISION_MODEL="gemma3:4b" \
    "$R2_EXEC"

status=$?

# Avoid leaving a stale observer window or X-server permission after R2 exits.
if [ -n "${OBSERVER_PID:-}" ]; then
    kill "$OBSERVER_PID" 2>/dev/null || true
fi
if [ "${OBSERVER_XHOST_GRANTED:-0}" -eq 1 ]; then
    xhost -SI:localuser:r2 >/dev/null 2>&1 || true
fi

echo
echo "========================================"
echo "R2 exited with status: $status"
echo "========================================"
echo

read -p "Press Enter to close..."
exit $status
