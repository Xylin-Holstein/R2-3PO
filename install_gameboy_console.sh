#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="\${R2_GAMEBOY_DIR:-/home/x/R2_Home/Devices/GameBoyAdvance}"

mkdir -p "$TARGET_DIR/Cartridges" "$TARGET_DIR/Saves" "$TARGET_DIR/State"
install -m 0755 "$SOURCE_DIR/GameBoyAdvance.py" "$TARGET_DIR/GameBoyAdvance"

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
