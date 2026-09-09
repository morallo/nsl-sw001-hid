#!/usr/bin/env bash
#
# Boot-time re-assert script for the N-SL SW001 HID-BPF program on SteamOS.
#
# Persistence model:
#   - SteamOS replaces /usr (root) and prunes /etc to the atomic-update
#     keep-list on every A/B update.  /home is never touched.
#   - The loader binary, its bundled .so deps and the BPF object live under
#     /home/.steamos-nsl-sw001-bpf/ and stay fresh after every update -- no
#     pacman, no steamos-readonly unlock, nothing to reinstall.
#   - This script only re-asserts the /etc copies (udev rule, keep-list,
#     ensure unit) from the /home seed in case a prune or factory reset
#     dropped them, and re-attaches the program to a present controller.
#
# The unit (steamos-nsl-sw001-bpf.service) is preserved by the atomic-update
# keep-list, so it keeps running across updates.

set -euo pipefail

SEED="/home/.steamos-nsl-sw001-bpf"
LOADER="sw001-bpf-attach"
OBJ="nsl-sw001.bpf.o"
SVC="steamos-nsl-sw001-bpf.service"
KEEPLIST_CONF="nsl-sw001-bpf.conf"
ENSURE_SH="steamos-nsl-sw001-bpf-ensure.sh"
RULE="99-hid-bpf-nsl-sw001.rules"

log() { echo "[nsl-sw001-bpf-ensure] $*"; }

if [ ! -x "$SEED/$LOADER" ] || [ ! -r "$SEED/$OBJ" ]; then
    log "seed missing at $SEED; nothing to do"
    exit 0
fi

# --- Helper to run privileged commands ---------------------------------------
run_priv() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo -n "$@"
    fi
}

if [ ! -r /sys/kernel/btf/vmlinux ]; then
    log "WARNING: /sys/kernel/btf/vmlinux missing; this kernel cannot load HID-BPF programs"
fi

# --- 1. Re-assert /etc copies from the seed ----------------------------------
log "re-asserting udev rule"
run_priv cp "$SEED/$RULE" "/etc/udev/rules.d/$RULE"

if [ ! -f "/etc/atomic-update.conf.d/$KEEPLIST_CONF" ]; then
    log "re-asserting atomic-update keep-list"
    run_priv mkdir -p /etc/atomic-update.conf.d
    run_priv cp "$SEED/$KEEPLIST_CONF" "/etc/atomic-update.conf.d/$KEEPLIST_CONF"
fi

run_priv cp "$SEED/$SVC" "/etc/systemd/system/$SVC"
run_priv systemctl daemon-reload
run_priv systemctl enable "$SVC" >/dev/null 2>&1 || true

run_priv udevadm control --reload || true

# --- 2. Remove any lingering kernel-module route artifacts -------------------
# The module route's global hid-nintendo blacklist would block the BPF route;
# a fresh A/B update can resurrect it from the module route's own keep-list.
if [ -f /etc/modprobe.d/blacklist-hid-nintendo.conf ]; then
    log "removing old hid-nintendo blacklist (module route)"
    run_priv rm -f /etc/modprobe.d/blacklist-hid-nintendo.conf
fi
if [ -f /etc/modules-load.d/nsl-sw001.conf ]; then
    log "removing old modules-load conf (module route)"
    run_priv rm -f /etc/modules-load.d/nsl-sw001.conf
fi

# --- 3. Attach to any already-connected controller ---------------------------
# The udev rule covers future connects; attach now in case the controller is
# already paired and powered on.  (If it was connected before the rule existed,
# power-cycle it once so hid-nintendo binds with the rewritten descriptor.)
for d in /sys/bus/hid/devices/0005:057E:2009.*; do
    [ -d "$d" ] || continue
    run_priv "$SEED/$LOADER" "$d" "$SEED/$OBJ" >/dev/null 2>&1 \
        && log "attached to $(basename "$d")" \
        || log "attach to $(basename "$d") deferred (udev rule handles it)"
done

log "done"