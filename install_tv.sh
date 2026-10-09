#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
R2_HOME="${R2_HOME:-/home/x/R2_Home}"
TV_DIR="${R2_TV_DIR:-$R2_HOME/room/TV}"
MEDIA_DIR="${R2_VCR_MEDIA_DIR:-$R2_HOME/VCR_Tapes}"

if [[ ! -f "$SOURCE_DIR/TV.py" ]]; then
    echo "TV.py was not found beside this installer." >&2
    exit 1
fi

mkdir -p "$TV_DIR" "$MEDIA_DIR"
install -m 0755 "$SOURCE_DIR/TV.py" "$TV_DIR/TV.py"

cat > "$TV_DIR/run_tv.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export R2_REALITY_DB="\${R2_REALITY_DB:-$R2_HOME/R2/r2_reality.db}"
export R2_TV_SOCKET="\${R2_TV_SOCKET:-$R2_HOME/R2/tv-control.sock}"
export R2_VCR_MEDIA_DIR="\${R2_VCR_MEDIA_DIR:-$MEDIA_DIR}"
# The R2 shell owns the private Reality database and mode-0600 control socket.
# Run the GUI as that same account; R2_Launch_Code.sh grants r2 access to the
# current X display while R2 is running. Keep the terminal attached so sudo
# can request authentication if its credential cache has expired.
if ! id r2 >/dev/null 2>&1; then
    echo "Linux user 'r2' does not exist; start R2's normal setup first." >&2
    exit 1
fi
exec sudo -u r2 env -u XAUTHORITY \
    DISPLAY="\${DISPLAY:-}" \
    R2_REALITY_DB="\$R2_REALITY_DB" \
    R2_TV_SOCKET="\$R2_TV_SOCKET" \
    R2_VCR_MEDIA_DIR="\$R2_VCR_MEDIA_DIR" \
    /usr/bin/env python3 "$TV_DIR/TV.py"
EOF
chmod 0755 "$TV_DIR/run_tv.sh"

cat > "$TV_DIR/TV.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=R2's CRT Television
Comment=Operate the CRT television and built-in VCR in R2's room
Exec=/bin/bash "$TV_DIR/run_tv.sh"
Path=$TV_DIR
Terminal=true
Categories=AudioVideo;Video;
EOF
chmod 0755 "$TV_DIR/TV.desktop"

cat > "$TV_DIR/device.txt" <<EOF
Object: TV
Location: Room
Type: CRT television with built-in VCR
Reality database: $R2_HOME/R2/r2_reality.db
Control socket: $R2_HOME/R2/tv-control.sock
VCR media directory: $MEDIA_DIR
State authority: R2 Reality; this file is descriptive metadata only.
EOF

echo "Installed the TV device files in: $TV_DIR"
echo "VCR media directory: $MEDIA_DIR"
echo "The R2 shell must be running before the GUI can change TV state."
echo "Open TV.desktop from R2's room to use the device."
