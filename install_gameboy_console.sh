#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${R2_GAMEBOY_DIR:-/home/x/R2_Home/Devices/GameBoyAdvance}"
POCKET_DIR="${R2_POCKET_DIR:-/home/x/R2_Home/pockets}"
DESKTOP_DIR="${R2_DESKTOP_DIR:-/home/x/.local/share/applications}"
MGBA_PATH="${R2_MGBA_EXECUTABLE:-/usr/games/mgba-qt}"

mkdir -p "$TARGET_DIR/Cartridges" "$TARGET_DIR/Saves" "$TARGET_DIR/State" "$POCKET_DIR" "$DESKTOP_DIR"
# Catch syntax problems before replacing the installed console executable.
python3 -m py_compile "$SOURCE_DIR/GameBoyAdvance.py"
install -m 0755 "$SOURCE_DIR/GameBoyAdvance.py" "$TARGET_DIR/GameBoyAdvance"
test -x "$TARGET_DIR/GameBoyAdvance"

# Keep a desktop-entry copy in R2's portable pocket filesystem as requested,
# as well as the standard applications directory for normal desktop launching.
sed "s|^Exec=.*$|Exec=$TARGET_DIR/GameBoyAdvance|" \
  "$SOURCE_DIR/GameBoyAdvance.desktop" > "$DESKTOP_DIR/GameBoyAdvance.desktop"
install -m 0644 "$DESKTOP_DIR/GameBoyAdvance.desktop" "$POCKET_DIR/GameBoyAdvance.desktop"
grep -Fx "Exec=$TARGET_DIR/GameBoyAdvance" "$DESKTOP_DIR/GameBoyAdvance.desktop" >/dev/null
grep -Fx "Exec=$TARGET_DIR/GameBoyAdvance" "$POCKET_DIR/GameBoyAdvance.desktop" >/dev/null

# Verify the actual emulator path without silently substituting another binary.
if [[ -x "$MGBA_PATH" ]]; then
  echo "Verified executable mGBA path: $MGBA_PATH"
else
  echo "WARNING: mGBA is not executable at $MGBA_PATH."
  echo "Set R2_MGBA_EXECUTABLE to the installed mGBA executable before powering on."
fi

# R2 is launched under the separate Linux account "r2". Give that account
# write access to device state/saves and read access to the pocket launcher.
if getent group r2 >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
  if sudo chgrp -R r2 "$TARGET_DIR"; then
    sudo find "$TARGET_DIR" -type d -exec chmod 2775 {} +
    sudo find "$TARGET_DIR" -type f -exec chmod g+rw {} +
    sudo chgrp r2 "$POCKET_DIR/GameBoyAdvance.desktop"
    sudo chmod 0644 "$POCKET_DIR/GameBoyAdvance.desktop"
  else
    echo "WARNING: group permissions were not changed."
    echo "If R2 runs as Linux user r2, grant that account write access to $TARGET_DIR."
  fi
else
  echo "WARNING: could not configure the r2 group automatically."
  echo "If R2 runs as a separate Linux account, grant it access to the device directory."
fi

cat <<EOF
Installed R2's Game Boy Advance console:
  Console executable: $TARGET_DIR/GameBoyAdvance
  Cartridge slot DB:  $TARGET_DIR/gameboy.db (created on first launch)
  Loose cartridges:  $TARGET_DIR/Cartridges/
  Normal game saves: $TARGET_DIR/Saves/
  Runtime/config:    $TARGET_DIR/State/
  Pocket launcher:   $POCKET_DIR/GameBoyAdvance.desktop
  Desktop launcher:  $DESKTOP_DIR/GameBoyAdvance.desktop

No cartridge is assumed to be owned or inserted. Once mGBA is installed,
verify the configured path with:
  R2_MGBA_EXECUTABLE="$MGBA_PATH" "$TARGET_DIR/GameBoyAdvance" --json verify
The external emulator remains at $MGBA_PATH by default.
EOF
