#!/usr/bin/env bash
# Unprivileged notice for the source checkout. Never run a writable source-tree
# installer with sudo: use the separately installed root-owned bootstrap instead.
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    echo "ERROR: do not execute this mutable source-tree script as root." >&2
    echo "Use the trusted root-owned entrypoint: /usr/local/sbin/r2-3po-install" >&2
    exit 1
fi

BOOTSTRAP="/usr/local/sbin/r2-3po-install"
if [[ ! -f "$BOOTSTRAP" || -L "$BOOTSTRAP" || ! -x "$BOOTSTRAP" ]]; then
    echo "ERROR: the trusted root-owned R2 installer has not been installed." >&2
    echo "Use the immutable-commit and Git blob hash verification procedure in:" >&2
    echo "  /home/x/R2_Home/R2/docs/overnight-service.md" >&2
    echo "Do not install the bootstrap directly from the runtime-writable R2 source tree." >&2
    exit 1
fi

owner="$(stat -c '%u' "$BOOTSTRAP")"
mode="$(stat -c '%a' "$BOOTSTRAP")"
if [[ "$owner" != "0" ]] || (( (8#$mode & 022) != 0 )); then
    echo "ERROR: $BOOTSTRAP must be owned by root and not group/world-writable." >&2
    exit 1
fi

echo "Trusted bootstrap is installed."
echo "Run the protected entrypoint directly (not this source-tree script with sudo):"
echo "  sudo /usr/local/sbin/r2-3po-install"
