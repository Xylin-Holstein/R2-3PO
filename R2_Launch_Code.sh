#!/bin/bash

R2_ROOT="/home/x/R2_Home"
R2_SOURCE="$R2_ROOT/R2"
R2_EXEC="$R2_ROOT/r2"
R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"
R2_OLLAMA_BASE_URL="${R2_OLLAMA_BASE_URL:-http://127.0.0.1:11434}"
if [[ "$R2_OLLAMA_BASE_URL" != "http://127.0.0.1:11434" ]]; then
    echo "ERROR: Interactive R2 must use its permitted local Ollama endpoint 127.0.0.1:11434."
    echo "Unset R2_OLLAMA_BASE_URL or set it to http://127.0.0.1:11434."
    exit 1
fi
export R2_OLLAMA_BASE_URL

# Keep R2's Linux identity separate from the canonical data location. HOME is
# set to the project root for every R2 process, never to /home/r2.
if ! id r2 >/dev/null 2>&1; then echo "ERROR: Dedicated Linux account r2 does not exist."; exit 1; fi
R2_UID="$(id -u r2)"
R2_GID="$(id -g r2)"
R2_HOME_DIR="$(getent passwd r2 | cut -d: -f6)"
if [[ ! "$R2_UID" =~ ^[0-9]+$ || ! "$R2_GID" =~ ^[0-9]+$ || -z "$R2_HOME_DIR" ]]; then
    echo "ERROR: Could not resolve the dedicated r2 account identity."
    exit 1
fi

R2_NETWORK_LOCK_ACTIVE=0
OBSERVER_XHOST_GRANTED=0
OBSERVER_PID=""

# Observer/display cleanup must run for every exit path, not only after R2
# returns normally. In particular, an early launch failure after opening the
# X-server permission must not leave a stale observer or grant behind.
r2_observer_cleanup() {
    if [[ -n "${OBSERVER_PID:-}" ]]; then
        kill "$OBSERVER_PID" 2>/dev/null || true
        wait "$OBSERVER_PID" 2>/dev/null || true
        OBSERVER_PID=""
    fi
    if [[ "${OBSERVER_XHOST_GRANTED:-0}" -eq 1 ]]; then
        xhost -SI:localuser:r2 >/dev/null 2>&1 || true
        OBSERVER_XHOST_GRANTED=0
    fi
}

r2_remove_network_jumps() {
    local binary="$1" chain="$2"
    while sudo -n "$binary" -w -C OUTPUT -m owner --uid-owner "$R2_UID" -j "$chain" >/dev/null 2>&1; do
        sudo -n "$binary" -w -D OUTPUT -m owner --uid-owner "$R2_UID" -j "$chain" || return 1
    done
}
r2_remove_network_family() {
    local binary="$1" chain="$2"
    command -v "$binary" >/dev/null 2>&1 || return 0
    r2_remove_network_jumps "$binary" "$chain" || true
    if sudo -n "$binary" -w -S "$chain" >/dev/null 2>&1; then
        sudo -n "$binary" -w -F "$chain" || return 1
        sudo -n "$binary" -w -X "$chain" || return 1
    fi
}
r2_remove_network_lock() {
    r2_remove_network_family iptables R2_3PO_UID_NETLOCK || return 1
    r2_remove_network_family ip6tables R2_3PO_UID_NETLOCK || return 1
}
r2_network_cleanup() {
    local exit_status=$?
    trap - EXIT
    r2_observer_cleanup
    if [[ "$R2_NETWORK_LOCK_ACTIVE" -eq 1 ]]; then
        if r2_remove_network_lock; then
            echo "[Network] R2-only network lock removed."
        else
            echo "WARNING: Could not fully remove R2-only network rules; inspect iptables/ip6tables." >&2
        fi
    fi
    exit "$exit_status"
}
trap r2_network_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

r2_apply_network_lock() {
    command -v iptables >/dev/null 2>&1 || { echo "ERROR: iptables is missing; refusing to launch without R2 network isolation."; return 1; }
    command -v ip6tables >/dev/null 2>&1 || { echo "ERROR: ip6tables is missing; refusing to launch without IPv6 isolation."; return 1; }
    sudo -n true >/dev/null 2>&1 || { echo "ERROR: non-interactive sudo is required to apply the R2-only network lock."; return 1; }
    r2_remove_network_jumps iptables R2_3PO_UID_NETLOCK || return 1
    r2_remove_network_jumps ip6tables R2_3PO_UID_NETLOCK || return 1
    sudo -n iptables -w -S R2_3PO_UID_NETLOCK >/dev/null 2>&1 || sudo -n iptables -w -N R2_3PO_UID_NETLOCK || return 1
    sudo -n ip6tables -w -S R2_3PO_UID_NETLOCK >/dev/null 2>&1 || sudo -n ip6tables -w -N R2_3PO_UID_NETLOCK || return 1
    sudo -n iptables -w -F R2_3PO_UID_NETLOCK || return 1
    sudo -n ip6tables -w -F R2_3PO_UID_NETLOCK || return 1
    # Permit only local Ollama TCP. No wireless interface is disabled and the
    # OUTPUT owner match leaves the desktop user's network traffic untouched.
    sudo -n iptables -w -A R2_3PO_UID_NETLOCK -o lo -d 127.0.0.1/32 -p tcp --dport 11434 -j ACCEPT || return 1
    sudo -n iptables -w -A R2_3PO_UID_NETLOCK -m conntrack --ctstate ESTABLISHED -o lo -s 127.0.0.1/32 -p tcp --sport 11434 -j ACCEPT || return 1
    sudo -n iptables -w -A R2_3PO_UID_NETLOCK -j REJECT || return 1
    sudo -n ip6tables -w -A R2_3PO_UID_NETLOCK -j REJECT || return 1
    sudo -n iptables -w -I OUTPUT 1 -m owner --uid-owner "$R2_UID" -j R2_3PO_UID_NETLOCK || return 1
    if ! sudo -n ip6tables -w -I OUTPUT 1 -m owner --uid-owner "$R2_UID" -j R2_3PO_UID_NETLOCK; then
        r2_remove_network_lock
        return 1
    fi
    R2_NETWORK_LOCK_ACTIVE=1
    echo "[Network] Linux user r2 may access only local Ollama at 127.0.0.1:11434."
}


# On this host the GTX 1050 Ti has 4 GiB VRAM. Request a partial layer
# offload by default instead of forcing the entire model into limited VRAM.
# An explicit environment value always wins; 0 leaves placement automatic.
if [[ -z "${R2_OLLAMA_NUM_GPU+x}" ]]; then
    if command -v nvidia-smi >/dev/null 2>&1 &&
       nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | grep -qi 'GTX 1050 Ti'; then
        export R2_OLLAMA_NUM_GPU=20
        echo "[GPU] NVIDIA GeForce GTX 1050 Ti detected; requesting 20 model layers on GPU."
    else
        export R2_OLLAMA_NUM_GPU=0
        echo "[GPU] GTX 1050 Ti not detected; leaving placement automatic."
    fi
else
    export R2_OLLAMA_NUM_GPU
    echo "[GPU] Using configured R2_OLLAMA_NUM_GPU=$R2_OLLAMA_NUM_GPU."
fi

# The interactive launcher and the overnight service share persistent databases.
# Never start a second core against those same files.
if command -v systemctl >/dev/null 2>&1 &&
   systemctl is-active --quiet r2-3po.service 2>/dev/null; then
    echo "ERROR: R2's overnight service is already running."
    echo "Stop it with: sudo systemctl stop r2-3po.service"
    echo "This prevents two R2 cores from writing the same databases concurrently."
    exit 1
fi

# The service check alone does not catch a second interactive launcher.
if ! command -v pgrep >/dev/null 2>&1; then
    echo "ERROR: pgrep is required to check for an existing R2 core."
    exit 1
fi
if pgrep -x r2 >/dev/null 2>&1; then
    echo "ERROR: An R2 core is already running."
    echo "Stop the existing R2 session before launching another; both would write the same databases."
    exit 1
fi

echo "[Network] Applying R2-only network isolation..."
if ! r2_apply_network_lock; then
    r2_remove_network_lock
    echo "ERROR: R2 network isolation could not be verified; R2 was NOT launched."
    exit 1
fi
echo

# Run the read-only deployment preflight before touching workspace state,
# compiling, installing packages, starting the observer, or launching R2.
R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"
echo "========================================"
echo "       R2-3PO DEPLOYMENT PREFLIGHT"
echo "========================================"
echo
if [[ ! -r "$R2_PREFLIGHT" ]]; then
    echo "ERROR: Read-only deployment preflight is missing:"
    echo "       $R2_PREFLIGHT"
    echo "Copy tools/preflight_r2_hardware.sh into the R2 source tree first."
    if [[ -t 0 ]]; then read -r -p "Press Enter to exit..."; fi
    exit 1
fi
if ! R2_ROOT="$R2_ROOT" R2_SOURCE="$R2_SOURCE" R2_EXEC="$R2_EXEC" \
     R2_FX_DIR="$R2_FX_DIR" bash "$R2_PREFLIGHT"; then
    echo
    echo "R2 launch cancelled: preflight reported one or more failures."
    echo "Correct the [FAIL] items above, then run the launcher again."
    if [[ -t 0 ]]; then read -r -p "Press Enter to exit..."; fi
    exit 1
fi
echo
echo "Preflight finished. Review any [WARN] items above."
if [[ -t 0 ]]; then
    read -r -p "Press Enter to continue launching R2, or Ctrl+C to cancel..."
fi
echo

echo "========================================"
echo "        R2-3PO LAUNCH SEQUENCE"
echo "========================================"
echo

if [[ ! -d "$R2_ROOT" ]]; then
    echo "ERROR: R2 workspace root does not exist: $R2_ROOT"
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
    "$R2_ROOT/R2_Log"
    "$R2_ROOT/room"
    "$R2_ROOT/room/shelf"
    "$R2_ROOT/room/box"
    "$R2_ROOT/room/piggybank"
    "$R2_ROOT/Pockets"
    "$R2_ROOT/Pockets/Wallet"
    "$R2_ROOT/fridge"
)
for DIR in "${R2_DIRS[@]}"; do
    # Runtime-owned directories must be created as r2. Creating a missing
    # directory as the desktop login user would leave it mode 0755 and
    # unwritable by R2, causing SQLite/diary/world-state writes to fail later.
    if ! sudo -n -u r2 mkdir -p "$DIR"; then
        echo "ERROR: Could not create workspace directory as r2: $DIR"
        echo "Check that r2 can traverse and write the workspace root."
        read -p "Press Enter to exit..."
        exit 1
    fi
done
echo "Workspace directories are ready."
echo

# Fail early unless the local multimodal model completes a real generation.
# The shared check is localhost-only and does not download models or contact the internet.
echo "[preflight] Checking the local Ollama API and required model..."
if [[ ! -r "$R2_SOURCE/tools/preflight_ollama_model.py" ]]; then
    echo "ERROR: shared Ollama inference preflight is missing."
    echo "R2 was NOT launched."
    read -p "Press Enter to exit..."
    exit 1
fi
if ! sudo -n -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 \
    R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
    R2_OLLAMA_NUM_GPU="$R2_OLLAMA_NUM_GPU" \
    R2_OLLAMA_TIMEOUT_SECONDS="${R2_OLLAMA_TIMEOUT_SECONDS:-}" \
    python3 "$R2_SOURCE/tools/preflight_ollama_model.py" --timeout 180; then
    echo
    echo "R2 was NOT launched."
    echo "Start the local Ollama service and ensure 'ollama list' includes qwen3.5:9b."
    echo "If the model is missing, run: ollama pull qwen3.5:9b"
    echo "Then launch R2 again."
    read -p "Press Enter to exit..."
    exit 1
fi
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
        if ! sudo -n -u r2 -- env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 \
            R2_ROOT="$R2_ROOT" R2_SOURCE="$R2_SOURCE" \
            bash "$TV_INSTALLER"; then
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
    "$R2_SOURCE/V-Webcam.py"

    "$R2_SOURCE/R2Sounds.c"
    "$R2_SOURCE/R2Sounds.h"

    "$R2_SOURCE/R2Reply.c"
    "$R2_SOURCE/R2Reply.h"
    "$R2_SOURCE/Imagination.c"
    "$R2_SOURCE/Imagination.h"
    "$R2_SOURCE/SensoryJournal.c"
    "$R2_SOURCE/SensoryJournal.h"

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

# Sound assets are read by the separate r2 Linux account. Check access as that
# account now, so a /home permission mismatch is reported before conversation
# markers silently fail. An absent collection is non-fatal to R2 itself.
R2_FX_DIR="${R2_FX_DIR:-/home/x/R2_Home/R2_sounds/FX}"
SOUND_AUDIT="$R2_SOURCE/tools/audit_r2_sound_assets.py"
if [ -f "$SOUND_AUDIT" ]; then
    if ! sudo -n -u r2 env HOME="$R2_ROOT" USER=r2 LOGNAME=r2 \
            python3 "$SOUND_AUDIT" --root "$R2_FX_DIR"; then
        echo "WARNING: r2 could not complete the sound asset inventory."
        echo "R2 will still start; beep/whistle markers may have no matching audio."
    fi
elif sudo -n -u r2 env HOME="$R2_ROOT" bash -c 'test -d "$1" && test -r "$1" && find "$1" -maxdepth 1 -type f -iname "*.mp3" -print -quit | grep -q .' _ "$R2_FX_DIR"; then
    echo "State-aware sound assets are readable by r2: $R2_FX_DIR"
else
    echo "WARNING: r2 cannot find readable MP3 effects in: $R2_FX_DIR"
    echo "R2 will still start; beep/whistle markers may have no matching audio."
fi
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
# R2 PRIVATE CONTINUOUS CLOCK
#
# Keep one fixed timestamp offset. Recomputing an offset from "current date
# with year 1999" on every launch silently rewinds the clock at year rollover.
# This offset maps the 2026-10-13 baseline to 1999-10-13 and then lets the
# virtual clock advance naturally into 2000, 2001, and beyond, including across
# process restarts. Only R2 receives the offset; the host clock is untouched.
# ------------------------------------------------------------

REAL_EPOCH="$(date '+%s')"
if [[ ! "$REAL_EPOCH" =~ ^-?[0-9]+$ ]]; then
    echo "ERROR: Could not read the real system epoch."
    echo "R2 was NOT launched."
    exit 1
fi
REAL_NOW="$(date -d "@$REAL_EPOCH" '+%Y-%m-%d %H:%M:%S')"
FAKETIME_OFFSET=-852076800
R2_EPOCH=$((REAL_EPOCH + FAKETIME_OFFSET))
if ! R2_START="$(date -d "@$R2_EPOCH" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" || [[ -z "$R2_START" ]]; then
    echo "ERROR: Could not calculate R2 virtual time from the fixed clock offset."
    echo "R2 was NOT launched."
    exit 1
fi

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
echo "    PRIVATE / CONTINUOUS (fixed offset; no relaunch reset)"
echo
echo "The Linux system clock will NOT be changed."
echo "R2's clock will roll naturally through year boundaries and remain continuous on relaunch."
echo

echo "[3/4] Recompiling R2..."
echo

# Build beside the live executable and replace it only after a successful link.
# This preserves the last known-good R2 binary if compilation fails.
TEMP_EXEC="${R2_EXEC}.tmp.${BASHPID}"
if [ -e "$TEMP_EXEC" ] || [ -L "$TEMP_EXEC" ]; then
    echo "ERROR: Temporary build path already exists: $TEMP_EXEC"
    echo "R2 was NOT launched."
    exit 1
fi

sudo -n -u r2 -- gcc -std=c11 -Wall -Wextra -Werror -O2 \
    "$R2_SOURCE/r2.c" \
    "$R2_SOURCE/shell.c" \
    "$R2_SOURCE/r2_diary.c" \
    "$R2_SOURCE/Log.c" \
    "$R2_SOURCE/Reality.c" \
    "$R2_SOURCE/Addiction.c" \
    "$R2_SOURCE/Reward.c" \
    "$R2_SOURCE/AlternateSelf.c" \
    "$R2_SOURCE/Imagination.c" \
    "$R2_SOURCE/SensoryJournal.c" \
    "$R2_SOURCE/Visual.c" \
    "$R2_SOURCE/Ears.c" \
    "$R2_SOURCE/Eyes.c" \
    "$R2_SOURCE/R2Sounds.c" \
    "$R2_SOURCE/R2Reply.c" \
    -o "$TEMP_EXEC" \
    -lcurl \
    -lsqlite3 \
    -lpthread \
    -ljson-c \
    -lpulse-simple \
    -lpulse \
    -lm

status=$?

if [ $status -ne 0 ]; then
    sudo -n -u r2 -- rm -f "$TEMP_EXEC"
    echo
    echo "========================================"
    echo "ERROR: R2 compilation failed."
    echo "The previous executable was preserved; R2 was NOT launched."
    echo "========================================"
    echo
    read -p "Press Enter to exit..."
    exit $status
fi

if ! sudo -n -u r2 -- chmod 0755 "$TEMP_EXEC" || ! sudo -n -u r2 -- mv -f "$TEMP_EXEC" "$R2_EXEC"; then
    sudo -n -u r2 -- rm -f "$TEMP_EXEC"
    echo "ERROR: Could not atomically install the newly compiled R2 executable."
    echo "The previous executable was preserved if replacement had not completed; R2 was NOT launched."
    exit 1
fi

echo
echo "Compilation successful."
echo "Executable updated:"
echo "    $R2_EXEC"
echo
echo "Unified conversation + vision model: qwen3.5:9b"
echo "R2 uses one shared Ollama model for text and image perception."
echo "If needed, install it with: ollama pull qwen3.5:9b"
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
echo "    R2Reply.c"
echo "    Imagination.c"
echo "    SensoryJournal.c"
echo

echo "[4/4] Transitioning to Linux user: r2..."
echo

echo "Launching the read-only observer window..."
echo

# The observer must run as r2 to read the private Life Log database, but
# the desktop X server normally rejects that separate Linux user. Grant only
# r2 access to this local X server, then revoke it when R2 exits.
if [ -n "${DISPLAY:-}" ] && command -v xhost >/dev/null 2>&1 && xhost +SI:localuser:r2 >/dev/null 2>&1; then
    OBSERVER_XHOST_GRANTED=1
    # Don't pass the desktop user's cookie or R2's virtual-clock environment.
    sudo -n -u r2 env -u XAUTHORITY HOME="$R2_ROOT" USER=r2 LOGNAME=r2 DISPLAY="${DISPLAY}" \
        python3 "$R2_SOURCE/Observer.py" &
    OBSERVER_PID=$!
else
    echo "WARNING: Could not authorize r2 for the desktop display; skipping Observer window."
    echo "R2 will still launch normally."
fi

echo "Launching R2..."
echo

# Give this R2 process the audio device group without permanently changing
# account membership. Preserve every supplementary group already assigned
# to r2, then add the host's audio GID when that group exists.
R2_RUNTIME_GROUPS="$(id -G r2 | tr ' ' ',')"
if AUDIO_GID="$(getent group audio 2>/dev/null | cut -d: -f3)" &&
   [[ "$AUDIO_GID" =~ ^[0-9]+$ ]]; then
    case ",$R2_RUNTIME_GROUPS," in
        *,"$AUDIO_GID",*) ;;
        *) R2_RUNTIME_GROUPS+=",${AUDIO_GID}" ;;
    esac
fi

# ------------------------------------------------------------
# START R2 WITH ITS PRIVATE CLOCK
# ------------------------------------------------------------

R2_UID="$(id -u r2)"
R2_GID="$(id -g r2)"
R2_HOME_DIR="$(getent passwd r2 | cut -d: -f6)"
if [[ ! "$R2_UID" =~ ^[0-9]+$ || ! "$R2_GID" =~ ^[0-9]+$ || -z "$R2_HOME_DIR" ]]; then
    echo "ERROR: Could not resolve the dedicated r2 account identity."
    exit 1
fi

# Ubuntu sudo-rs does not support sudo's traditional -G option. Run setpriv
# with elevated privilege only long enough to set supplementary groups, then
# drop UID/GID before starting R2. No permanent account/group changes occur.
sudo -n setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" -- \
    env \
    HOME="$R2_ROOT" \
    USER="r2" \
    LOGNAME="r2" \
    DISPLAY="${DISPLAY:-}" \
    R2_SOURCE="$R2_SOURCE" \
    XAUTHORITY="${XAUTHORITY:-}" \
    LD_PRELOAD="$FAKETIME_LIB" \
    FAKETIME="${FAKETIME_OFFSET}" \
    FAKETIME_DONT_RESET=1 \
    R2_OLLAMA_BASE_URL="$R2_OLLAMA_BASE_URL" \
    R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}" \
    R2_OLLAMA_TIMEOUT_SECONDS="${R2_OLLAMA_TIMEOUT_SECONDS:-}" \
    R2_OLLAMA_NUM_GPU="${R2_OLLAMA_NUM_GPU}" \
    R2_VISION_MODEL="qwen3.5:9b" \
    R2_FX_DIR="$R2_FX_DIR" \
    sh -c 'exec "$1" 2>>"$2"' _ "$R2_EXEC" "$R2_ROOT/R2_Log/Debug.txt"

status=$?

# Also clean up immediately after a normal R2 exit; the EXIT trap is an
# idempotent fallback for all early exits and signals.
r2_observer_cleanup

echo
echo "========================================"
echo "R2 exited with status: $status"
echo "========================================"
echo

read -p "Press Enter to close..."
exit $status
