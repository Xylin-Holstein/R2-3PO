#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${R2_GAMEBOY_DIR:-/home/x/R2_Home/Devices/GameBoyAdvance}"

mkdir -p "$TARGET_DIR/Cartridges" "$TARGET_DIR/Saves" "$TARGET_DIR/State"
# Catch syntax problems before replacing the installed console executable.
python3 -m py_compile "$SOURCE_DIR/GameBoyAdvance.py"
install -m 0755 "$SOURCE_DIR/GameBoyAdvance.py" "$TARGET_DIR/GameBoyAdvance"

# Install a real desktop-entry launcher. Do not execute the .desktop file as a
# shell script; desktop environments read it as application metadata.
DESKTOP_DIR="${R2_DESKTOP_DIR:-/home/x/.local/share/applications}"
DESKTOP_ENTRY="$DESKTOP_DIR/GameBoyAdvance.desktop"
mkdir -p "$DESKTOP_DIR"
sed "s|^Exec=.*$|Exec=$TARGET_DIR/GameBoyAdvance|" \
  "$SOURCE_DIR/GameBoyAdvance.desktop" > "$DESKTOP_ENTRY"
chmod 0644 "$DESKTOP_ENTRY"

# Verify the selected emulator path without silently substituting another one.
MGBA_PATH="${R2_MGBA_EXECUTABLE:-/usr/games/mgba-qt}"
if [[ -x "$MGBA_PATH" ]]; then
  echo "Verified executable mGBA path: $MGBA_PATH"
else
  echo "WARNING: mGBA is not executable at $MGBA_PATH."
  echo "Set R2_MGBA_EXECUTABLE to the installed mGBA executable before powering on."
fi
"$TARGET_DIR/GameBoyAdvance" --json verify || true

# R2 is launched under the separate Linux account "r2". Give that account
# write access to its device state/saves while keeping the installer's account
# as owner so it can manage loose cartridge files normally.
if getent group r2 >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
  if sudo chgrp -R r2 "$TARGET_DIR"; then
    sudo find "$TARGET_DIR" -type d -exec chmod 2775 {} +
    sudo find "$TARGET_DIR" -type f -exec chmod g+rw {} +
  else
    echo "WARNING: group permissions were not changed."
    echo "If R2 runs as Linux user r2, grant that account write access to $TARGET_DIR."
  fi
else
  echo "WARNING: could not configure the r2 group automatically."
  echo "If R2 runs as Linux user r2, grant that account write access to $TARGET_DIR."
fi

cat <<EOF
Installed R2's Game Boy Advance console:
  Console executable: $TARGET_DIR/GameBoyAdvance
  Cartridge slot DB:  $TARGET_DIR/gameboy.db (created on first launch)
  Loose cartridges:  $TARGET_DIR/Cartridges/
  Normal game saves: $TARGET_DIR/Saves/
  Runtime/config:    $TARGET_DIR/State/

Desktop launcher:   /home/x/.local/share/applications/GameBoyAdvance.desktop\nThe external emulator remains at /usr/games/mgba-qt by default.
Override it with R2_MGBA_EXECUTABLE if its location differs.
If R2 runs as a separate Linux account, make sure that account can
read/execute the console and emulator and write to the device directory.
EOF
