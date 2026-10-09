#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

cat > "$TEMP_DIR/fake-mgba" <<'EOF'
#!/usr/bin/env sh
exit 0
EOF
chmod 0755 "$TEMP_DIR/fake-mgba"

R2_GAMEBOY_DIR="$TEMP_DIR/device" \
R2_DESKTOP_DIR="$TEMP_DIR/applications" \
R2_MGBA_EXECUTABLE="$TEMP_DIR/fake-mgba" \
  bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/install.log" 2>&1

test -x "$TEMP_DIR/device/GameBoyAdvance"
test -d "$TEMP_DIR/device/Cartridges"
test -d "$TEMP_DIR/device/Saves"
test -d "$TEMP_DIR/device/State"
test -f "$TEMP_DIR/applications/GameBoyAdvance.desktop"
grep -Fx "Exec=$TEMP_DIR/device/GameBoyAdvance" \
  "$TEMP_DIR/applications/GameBoyAdvance.desktop" >/dev/null

echo "Game Boy installer and desktop-entry smoke test passed."
