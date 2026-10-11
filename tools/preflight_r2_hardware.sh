#!/usr/bin/env bash
# Read-only deployment readiness report. Never installs, rebuilds, edits DBs, or starts/stops R2.
set -uo pipefail
R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"
R2_SOURCE="${R2_SOURCE:-$R2_ROOT/R2}"
R2_EXEC="${R2_EXEC:-$R2_ROOT/r2}"
R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"
OLLAMA_URL="${R2_OLLAMA_URL:-http://127.0.0.1:11434/api/tags}"
FAILURES=0
WARNINGS=0
pass() { printf '[PASS] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; WARNINGS=$((WARNINGS + 1)); }
fail() { printf '[FAIL] %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
section() { printf '\n== %s ==\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# sudo-rs (Ubuntu's Rust sudo) does not implement sudo's traditional -G flag.
# Use setpriv to install supplementary groups before dropping to r2. This is
# scoped to one process and never changes r2's persistent group membership.
run_as_r2_with_audio_group() {
  have setpriv || return 127
  local uid gid groups audio_gid
  uid="$(id -u r2)" || return 1
  gid="$(id -g r2)" || return 1
  groups="$(id -G r2 | tr ' ' ',')" || return 1
  audio_gid="$(getent group audio 2>/dev/null | cut -d: -f3 || true)"
  if [[ "$audio_gid" =~ ^[0-9]+$ ]]; then
    case ",$groups," in
      *,"$audio_gid",*) ;;
      *) groups+=",${audio_gid}" ;;
    esac
  fi
  sudo -n setpriv --reuid="$uid" --regid="$gid" --groups="$groups" -- "$@"
}

section "Host summary"
printf 'Workspace: %s\nSource: %s\nExecutable: %s\nFX assets: %s\n' "$R2_ROOT" "$R2_SOURCE" "$R2_EXEC" "$R2_FX_DIR"
if have lscpu; then lscpu | awk -F: '/^Model name:/ {gsub(/^[ \t]+/, "", $2); print "CPU: " $2; exit}'; fi
if [[ -r /proc/meminfo ]]; then awk '/^MemTotal:/ {printf "RAM: %.1f GiB\n", $2/1048576; exit}' /proc/meminfo; fi
if [[ -e /dev/dri/renderD128 || -e /dev/dri/card0 ]]; then
  printf 'Graphics: DRM device detected; this does not prove Ollama GPU offload works.\n'
else
  warn "No DRM graphics device detected; expect CPU inference unless Ollama reports otherwise."
fi
if have nvidia-smi; then nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | sed 's/^/GPU: /' || warn "NVIDIA GPU query failed."
else printf 'GPU: no NVIDIA runtime detected; CPU inference may still work.\n'; fi

section "Required files and build tools"
# These commands are used directly by the interactive launcher or overnight installer.
for tool in bash make gcc curl python3 sudo setpriv pgrep date find grep cmp; do
  if have "$tool"; then pass "$tool installed"; else fail "$tool missing"; fi
done
if have xhost; then pass "xhost available for optional Observer GUI access"; else warn "xhost missing; Observer may not be able to grant the r2 account X display access."; fi

FAKETIME_FOUND=0
for candidate in \
  /usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1 \
  /usr/lib/x86_64-linux-gnu/libfaketime.so.1 \
  /usr/lib/faketime/libfaketime.so.1 \
  /usr/local/lib/faketime/libfaketime.so.1 \
  /usr/local/lib/libfaketime.so.1; do
  if [[ -r "$candidate" ]]; then FAKETIME_FOUND=1; break; fi
done
if [[ "$FAKETIME_FOUND" -eq 1 ]]; then
  pass "libfaketime runtime found"
else
  warn "libfaketime.so.1 is missing; interactive startup may attempt package installation, but overnight service installation cannot proceed until it is installed."
fi
# Mirror the complete source/header set required by the interactive launcher,
# so a partial manual copy is caught before the user is asked to continue.
for file in \
  Makefile \
  r2.c r2.h \
  r2_diary.c r2_diary.h \
  Log.c Log.h \
  Reality.c Reality.h \
  Addiction.c Addiction.h \
  Reward.c Reward.h \
  AlternateSelf.c AlternateSelf.h \
  Visual.c Visual.h \
  Ears.c Ears.h \
  Eyes.c Eyes.h \
  R2Sounds.c R2Sounds.h \
  R2Reply.c R2Reply.h \
  shell.c shell.h \
  Observer.py; do
  if [[ -r "$R2_SOURCE/$file" ]]; then pass "source readable: $file"; else fail "missing/unreadable: $R2_SOURCE/$file"; fi
done
for file in \
  deploy/r2-3po.service \
  tools/r2_service_runner.sh \
  tools/install_r2_overnight.sh \
  tools/audit_r2_databases.py \
  tools/audit_r2_sound_assets.py \
  tools/preflight_r2_hardware.sh; do
  if [[ -r "$R2_SOURCE/$file" ]]; then pass "deployment file readable: $file"; else fail "missing deployment file: $R2_SOURCE/$file"; fi
done
if [[ -x "$R2_EXEC" ]]; then pass "installed R2 executable exists"; else warn "installed executable missing; installer must build it before activation."; fi
if have systemctl && systemctl is-active --quiet r2-3po.service 2>/dev/null; then
  warn "overnight service is active; stop it before installation or database preflight."
elif have pgrep && pgrep -x r2 >/dev/null 2>&1; then
  warn "interactive R2 is running; stop it before installation or database preflight."
else printf '[INFO] no active R2 core detected\n'; fi

section "Dedicated user, workspace, and sound assets"
if id r2 >/dev/null 2>&1; then pass "Linux account r2 exists"; else fail "Linux account r2 missing"; fi
if [[ -d "$R2_ROOT" && -d "$R2_SOURCE" ]]; then
  if have sudo && id r2 >/dev/null 2>&1; then
    for path in "$R2_ROOT" "$R2_SOURCE"; do
      if run_as_r2_with_audio_group test -r "$path" && run_as_r2_with_audio_group test -x "$path" && run_as_r2_with_audio_group test -w "$path"; then
        pass "r2 can read/write/traverse $path"
      else
        fail "r2 must have read/write/traverse access to $path for runtime directories, SQLite, and persistent memory; correct ownership/permissions before launch"
      fi
    done
    # Use the same setpriv-based identity/group setup as the launcher. On
    # Ubuntu sudo-rs, plain `sudo -u r2` can evaluate access with a different
    # supplementary-group context than the actual runtime process. Missing
    # directories are okay: the launcher creates them as r2.
    for path in "$R2_ROOT/R2_Diary" "$R2_ROOT/room" "$R2_ROOT/room/shelf" "$R2_ROOT/room/box" "$R2_ROOT/room/piggybank" "$R2_ROOT/Pockets" "$R2_ROOT/Pockets/Wallet" "$R2_ROOT/fridge"; do
      [[ -e "$path" ]] || continue
      if run_as_r2_with_audio_group test -d "$path" && run_as_r2_with_audio_group test -r "$path" && run_as_r2_with_audio_group test -x "$path" && run_as_r2_with_audio_group test -w "$path"; then
        pass "r2 can read/write/traverse existing runtime directory $path"
      else
        fail "existing runtime directory is not read/write/traverse accessible to r2: $path"
      fi
    done
    for database in "$R2_SOURCE/r2_memory.db" "$R2_SOURCE/r2_reality.db" "$R2_SOURCE/r2_fridge.db" "$R2_SOURCE/r2_rewards.db" "$R2_SOURCE/r2_addictions.db"; do
      [[ -e "$database" ]] || continue
      if [[ -L "$database" ]]; then
        fail "refusing symlinked runtime database: $database"
      elif run_as_r2_with_audio_group test -f "$database" && run_as_r2_with_audio_group test -r "$database" && run_as_r2_with_audio_group test -w "$database"; then
        pass "r2 can read/write database $database"
      else
        fail "existing database must be a readable/writable regular file for r2: $database"
      fi
    done
  fi
else fail "workspace root or source directory missing"; fi
if [[ -d "$R2_FX_DIR" && -r "$R2_FX_DIR" ]]; then
  if find "$R2_FX_DIR" -maxdepth 1 -type f -iname '*.mp3' -print -quit 2>/dev/null | grep -q .; then
    pass "FX directory contains MP3 assets"
  else warn "FX directory contains no MP3 files; learned beeps/whistles cannot play."; fi
else warn "FX directory missing/unreadable; expected local assets at $R2_FX_DIR."; fi
if have sudo && id r2 >/dev/null 2>&1 && [[ -d "$R2_FX_DIR" ]]; then
  if run_as_r2_with_audio_group test -r "$R2_FX_DIR" && run_as_r2_with_audio_group test -x "$R2_FX_DIR"; then
    pass "r2 can read/traverse the FX directory"
  else warn "could not verify FX-directory access for r2 (sudo may require a password)."; fi
fi

section "Ollama and unified Gemma model"
if TAGS="$(curl --silent --show-error --fail --max-time 4 "$OLLAMA_URL" 2>/dev/null)"; then
  if printf '%s' "$TAGS" | python3 -c 'import json,sys; d=json.load(sys.stdin); sys.exit(0 if any(m.get("name") == "gemma3:4b" or m.get("name","").startswith("gemma3:4b-") for m in d.get("models",[])) else 1)' >/dev/null 2>&1; then
    pass "local Ollama API responds and gemma3:4b is installed"
  else fail "Ollama responds but gemma3:4b is not installed"; fi
else fail "Ollama API unreachable at $OLLAMA_URL"; fi
if have ollama; then printf 'Ollama placement (when a model is loaded):\n'; ollama ps 2>&1 | sed 's/^/  /' || warn "ollama ps could not report placement."
else warn "ollama CLI not found on PATH."; fi

section "Read-only SQLite integrity"
python3 - "$R2_SOURCE" <<'PY'
import pathlib, sqlite3, sys
from urllib.parse import quote
root = pathlib.Path(sys.argv[1])
names = ("r2_memory.db", "r2_reality.db", "r2_fridge.db", "r2_rewards.db", "r2_addictions.db")
existing = bad = 0
for name in names:
    path = root / name
    if path.is_symlink():
        print(f"[FAIL] refusing symlinked database: {path}"); bad += 1; continue
    if not path.exists():
        print(f"[INFO] not created yet: {name}"); continue
    existing += 1
    try:
        uri = "file:" + quote(str(path.resolve()), safe="/") + "?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=3) as db:
            result = db.execute("PRAGMA integrity_check").fetchone()
        if result and result[0] == "ok": print(f"[PASS] {name}: integrity_check=ok (read-only)")
        else: print(f"[FAIL] {name}: integrity_check={result!r}"); bad += 1
    except Exception as exc:
        print(f"[FAIL] {name}: read-only check failed: {exc}"); bad += 1
if not existing: print("[WARN] no existing database files; first-run DB initialization remains unverified")
sys.exit(1 if bad else 0)
PY
db_rc=$?
if [[ "$db_rc" -ne 0 ]]; then fail "existing database integrity check failed"; fi

section "Audio, microphone, camera, TV, and emulator"
if have mpg123 || have ffplay || have mpv; then pass "supported local MP3 playback process available"
else
  warn "no mpg123/ffplay/mpv; sound effects will not play. On Ubuntu/Debian, install a decoder with: sudo apt-get update && sudo apt-get install -y ffmpeg"
fi
if have pactl; then
  if pactl info >/dev/null 2>&1; then pass "desktop user's PulseAudio/PipeWire server responds"
  else warn "desktop user's PulseAudio/PipeWire server is unreachable."; fi
  if have sudo && id r2 >/dev/null 2>&1; then
    if sudo -n -u r2 pactl info >/dev/null 2>&1; then
      pass "R2 runtime account can connect to its default PulseAudio/PipeWire server"
    else
      warn "R2 runtime account cannot connect to its default PulseAudio/PipeWire server; desktop audio working for your login does not prove R2 can play sounds or capture monitor audio."
    fi
  fi
else
  warn "pactl missing; PulseAudio/PipeWire routing cannot be checked. On Ubuntu/Debian, install the diagnostic client with: sudo apt-get update && sudo apt-get install -y pulseaudio-utils"
fi
if have arecord; then arecord -l 2>&1 | sed 's/^/  /' || true; else warn "arecord missing; cannot enumerate microphone devices."; fi
if have aplay; then aplay -l 2>&1 | sed 's/^/  /' || true; else warn "aplay missing; cannot enumerate ALSA output devices."; fi
camera_count=0
camera_accessible=0
for camera in /dev/video*; do
  [[ -e "$camera" ]] || continue
  camera_count=$((camera_count + 1))
  printf '[INFO] camera: %s\n' "$camera"
  if [[ -r "$camera" && -w "$camera" ]]; then
    pass "current user can read/write $camera"
  else
    warn "current user cannot read/write $camera; camera capture may fail."
  fi
  if have sudo && id r2 >/dev/null 2>&1; then
    if run_as_r2_with_audio_group test -r "$camera" && run_as_r2_with_audio_group test -w "$camera"; then
      camera_accessible=$((camera_accessible + 1))
    fi
  fi
done
if [[ "$camera_count" -eq 0 ]]; then
  warn "no /dev/video* camera detected; this is a physical-device/configuration issue, not a missing package. Connect the intended webcam/capture device or configure Eyes to use its intended source."
elif have sudo && id r2 >/dev/null 2>&1; then
  if [[ "$camera_accessible" -gt 0 ]]; then
    pass "R2 runtime account can read/write $camera_accessible camera device(s)"
  else
    warn "R2 runtime account access to camera devices could not be verified; webcam capture may fail even if the desktop user can use the camera."
  fi
fi
sound_node_count=0
sound_node_accessible=0
sound_node_service_accessible=0
service_audio_group_configured=0
if [[ -r "$R2_SOURCE/deploy/r2-3po.service" ]] &&
   grep -Eq '^[[:space:]]*SupplementaryGroups=.*audio' "$R2_SOURCE/deploy/r2-3po.service"; then
  service_audio_group_configured=1
fi
for node in /dev/snd/*; do
  [[ -e "$node" ]] || continue
  sound_node_count=$((sound_node_count + 1))
  if have sudo && id r2 >/dev/null 2>&1; then
    if run_as_r2_with_audio_group test -r "$node" && run_as_r2_with_audio_group test -w "$node"; then
      sound_node_accessible=$((sound_node_accessible + 1))
    fi
    # Test the service's configured supplementary group without changing
    # account membership or touching device state.
    if [[ "$service_audio_group_configured" -eq 1 ]] &&
       run_as_r2_with_audio_group test -r "$node" &&
       run_as_r2_with_audio_group test -w "$node"; then
      sound_node_service_accessible=$((sound_node_service_accessible + 1))
    fi
  fi
done
if [[ "$sound_node_count" -gt 0 ]] && have sudo && id r2 >/dev/null 2>&1; then
  if [[ "$service_audio_group_configured" -eq 1 && "$sound_node_service_accessible" -gt 0 ]]; then
    pass "R2 service's configured audio supplementary group can access $sound_node_service_accessible ALSA device node(s); actual playback/capture still needs a runtime test"
  elif [[ "$sound_node_accessible" -gt 0 ]]; then
    pass "R2 runtime account can access $sound_node_accessible ALSA device node(s)"
  elif [[ "$service_audio_group_configured" -eq 1 ]]; then
    warn "service config grants the audio group, but access could not be verified for R2; direct ALSA playback/capture may fail."
  else
    warn "R2 runtime account cannot be verified to access ALSA device nodes; configure service-scoped audio access if direct ALSA playback/capture is intended."
  fi
elif [[ "$sound_node_count" -eq 0 ]]; then
  printf '[INFO] no /dev/snd device nodes; PulseAudio/PipeWire socket access may still provide audio.\n'
fi
for tool in ffmpeg ffprobe xdotool; do if have "$tool"; then pass "$tool available"; else warn "$tool missing; related media/controller feature may be degraded."; fi; done
if python3 -c 'import vlc, tkinter' >/dev/null 2>&1; then pass "Python VLC and Tkinter import"; else warn "Python VLC/Tkinter import failed; TV/VCR or Observer GUI may not open."; fi
if [[ -x /usr/games/mgba-qt ]]; then pass "mGBA Qt emulator available"; else warn "mGBA Qt not found at /usr/games/mgba-qt; configure R2_MGBA_EXECUTABLE if installed elsewhere."; fi
if [[ -r "$R2_SOURCE/TV.py" && -r "$R2_SOURCE/install_tv.sh" ]]; then pass "TV/VCR source and installer present"; else warn "TV/VCR source/installer missing."; fi

section "Summary"
printf 'Failures: %d  Warnings: %d\n' "$FAILURES" "$WARNINGS"
if [[ "$FAILURES" -gt 0 ]]; then
  printf 'NOT READY: fix every [FAIL] and rerun this read-only report.\n'; exit 1
elif [[ "$WARNINGS" -gt 0 ]]; then
  printf 'CORE CHECKS PASSED WITH WARNINGS. Review optional device/audio/GUI warnings before relying on those features.\n'
else
  printf 'PREFLIGHT PASSED. Still test a real conversation and physical speaker/microphone/camera after activation.\n'
fi
exit 0
