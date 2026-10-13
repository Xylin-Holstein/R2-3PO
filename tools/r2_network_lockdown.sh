#!/usr/bin/env bash
# UID-scoped network lock for R2. This never changes the host network interface
# and never matches the desktop account. Only packets owned by Linux user r2
# are filtered. IPv4 loopback TCP to/from Ollama's port is the sole IP exception.
set -euo pipefail

CHAIN4="R2_UID_NETLOCK"
CHAIN6="R2_UID_NETLOCK"
ACTION="${1:-}"

if [[ "$EUID" -ne 0 ]]; then
    echo "ERROR: network lock must be applied by root." >&2
    exit 1
fi
if ! id r2 >/dev/null 2>&1; then
    echo "ERROR: dedicated Linux account r2 does not exist." >&2
    exit 1
fi
R2_UID="$(id -u r2)"

remove_jump() {
    local binary="$1" chain="$2"
    while "$binary" -w -C OUTPUT -m owner --uid-owner "$R2_UID" -j "$chain" 2>/dev/null; do
        "$binary" -w -D OUTPUT -m owner --uid-owner "$R2_UID" -j "$chain"
    done
}

remove_family() {
    local binary="$1" chain="$2"
    command -v "$binary" >/dev/null 2>&1 || return 0
    remove_jump "$binary" "$chain" || true
    if "$binary" -w -S "$chain" >/dev/null 2>&1; then
        "$binary" -w -F "$chain" || return 1
        "$binary" -w -X "$chain" || return 1
    fi
}

apply_family4() {
    local binary="iptables"
    "$binary" -w -S "$CHAIN4" >/dev/null 2>&1 || "$binary" -w -N "$CHAIN4"
    remove_jump "$binary" "$CHAIN4"
    "$binary" -w -F "$CHAIN4"
    # Requests to the only permitted network service: local Ollama.
    "$binary" -w -A "$CHAIN4" -o lo -d 127.0.0.1/32 -p tcp --dport 11434 -j ACCEPT
    # Also allow the return direction if Ollama itself happens to run as r2.
    "$binary" -w -A "$CHAIN4" -o lo -s 127.0.0.1/32 -p tcp --sport 11434 -j ACCEPT
    "$binary" -w -A "$CHAIN4" -j REJECT
    "$binary" -w -I OUTPUT 1 -m owner --uid-owner "$R2_UID" -j "$CHAIN4"
}

apply_family6() {
    local binary="ip6tables"
    "$binary" -w -S "$CHAIN6" >/dev/null 2>&1 || "$binary" -w -N "$CHAIN6"
    remove_jump "$binary" "$CHAIN6"
    "$binary" -w -F "$CHAIN6"
    # Ollama is configured by R2 to use 127.0.0.1, not IPv6. Deny all IPv6
    # output for this UID, including ::1, so IPv6 cannot be a bypass.
    "$binary" -w -A "$CHAIN6" -j REJECT
    "$binary" -w -I OUTPUT 1 -m owner --uid-owner "$R2_UID" -j "$CHAIN6"
}

case "$ACTION" in
    apply)
        command -v iptables >/dev/null 2>&1 || { echo "ERROR: iptables is required; refusing to launch without network isolation." >&2; exit 1; }
        command -v ip6tables >/dev/null 2>&1 || { echo "ERROR: ip6tables is required; refusing to launch without IPv6 isolation." >&2; exit 1; }
        if ! apply_family4; then
            remove_family iptables "$CHAIN4" || true
            echo "ERROR: failed to install IPv4 R2 network lock." >&2
            exit 1
        fi
        if ! apply_family6; then
            remove_family iptables "$CHAIN4" || true
            remove_family ip6tables "$CHAIN6" || true
            echo "ERROR: failed to install IPv6 R2 network lock; IPv4 rules rolled back." >&2
            exit 1
        fi
        echo "R2 network lock active: UID $R2_UID may use only local Ollama TCP port 11434."
        ;;
    remove)
        remove_family iptables "$CHAIN4"
        remove_family ip6tables "$CHAIN6"
        echo "R2 network lock removed."
        ;;
    *)
        echo "Usage: $0 {apply|remove}" >&2
        exit 2
        ;;
esac
