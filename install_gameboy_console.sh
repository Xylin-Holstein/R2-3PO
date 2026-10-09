#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="\${R2_GAMEBOY_DIR:-/home/x/R2_Home/Devices/GameBoyAdvance}"

mkdir -p "$TARGET_DIR/Cartridges" "$TARGET_DIR/Saves" "$TARGET_DIR/State"
install -m 0755 "$SOURCE_DIR/GameBoyAdvance.py" "$TARGET_DIR/GameBoyAdvance"

# R2 is launched under the separate Linux account "r2". Give that account
# write access to its device state/saves while keeping the installer's account
# as owner so it can manage loose cartridge files normally.
if getent group r2 >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
  sudo chgrp -R r2 "$TARGET_DIR"
  sudo find "$TARGET_DIR" -type d -exec chmod 2775 {} +
  sudo find "$TARGET_DIR" -type f -exec chmod g+rw {} +
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

The external emulator remains at /usr/games/mgba-qt by default.
Override it with R2_MGBA_EXECUTABLE if its location differs.
If R2 runs as a separate Linux account, make sure that account can
read/execute the console and emulator and write to the device directory.
EOF
