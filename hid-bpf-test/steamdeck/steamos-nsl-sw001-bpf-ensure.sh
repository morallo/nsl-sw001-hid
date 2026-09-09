#!/usr/bin/env bash
#
# Self-seeding ensure script for the N-SL SW001 HID-BPF program on SteamOS.
#
# Persistence model:
#   - SteamOS replaces /usr (root) on every A/B update, wiping the udev-hid-bpf
#     loader, and /etc keeps only files on the atomic-update keep-list.  /home
#     is never touched.
#   - The BPF program is a single prebuilt object; no per-kernel rebuild is
#     needed (CO-RE relocations adapt it at load time).
#   - The ONLY reliable persistent copy of the program + scripts is under
#     /home/.steamos-nsl-sw001-bpf/.  This script re-installs the loader and
#     the udev rule + program from there on every boot.
#
# The unit (steamos-nsl-sw001-bpf.service) is preserved by the atomic-update
# keep-list, and this script also re-asserts the unit + enablement, so it keeps
# running across updates.

set -euo pipefail

SEED="/home/.steamos-nsl-sw001-bpf"
OBJ="nsl-sw001.bpf.o"
SVC="steamos-nsl-sw001-bpf.service"
KEEPLIST_CONF="nsl-sw001-bpf.conf"
ENSURE_SH="steamos-nsl-sw001-bpf-ensure.sh"
RULE="99-hid-bpf-nsl-sw001.rules"

log() { echo "[nsl-sw001-bpf-ensure] $*"; }

if [ ! -f "$SEED/$ENSURE_SH" ]; then
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

# --- Bring the rootfs writable (we need to touch /usr and /etc) --------------
refresh_readonly() {
    if command -v steamos-readonly >/dev/null 2>&1; then
        run_priv steamos-readonly disable
    fi
}

RO() {
    if command -v steamos-readonly >/dev/null 2>&1; then
        run_priv steamos-readonly enable || true
    fi
}

refresh_readonly
trap 'RO' EXIT

# --- 1. Loader ---------------------------------------------------------------
if ! command -v udev-hid-bpf >/dev/null 2>&1; then
    log "installing udev-hid-bpf loader"
    run_priv pacman-key --init >/dev/null 2>&1 || true
    run_priv pacman-key --populate archlinux >/dev/null 2>&1 || true
    run_priv pacman-key --populate holo >/dev/null 2>&1 || true
    run_priv pacman --noconfirm -S --needed udev-hid-bpf || true
fi
if ! command -v udev-hid-bpf >/dev/null 2>&1; then
    log "WARNING: udev-hid-bpf still missing; cannot load the program this boot"
    log "         (add the Arch extra repo or install it, then run:"
    log "         sudo udev-hid-bpf install --force $SEED/$OBJ)"
    exit 0
fi

if [ ! -r /sys/kernel/btf/vmlinux ]; then
    log "WARNING: /sys/kernel/btf/vmlinux missing; this kernel cannot load HID-BPF programs"
fi

# --- 2. Re-install the program + udev rule -----------------------------------
log "re-installing $OBJ via udev-hid-bpf"
run_priv udev-hid-bpf install --force "$SEED/$OBJ"
run_priv udevadm control --reload || true

# --- 3. Re-assert keep-list + unit + enablement ------------------------------
if [ ! -f "/etc/atomic-update.conf.d/$KEEPLIST_CONF" ]; then
    log "re-asserting atomic-update keep-list"
    run_priv mkdir -p /etc/atomic-update.conf.d
    run_priv cp "$SEED/etc/$KEEPLIST_CONF" "/etc/atomic-update.conf.d/$KEEPLIST_CONF"
fi
run_priv cp "$SEED/etc/$SVC" "/etc/systemd/system/$SVC"
run_priv systemctl daemon-reload
run_priv systemctl enable "$SVC" >/dev/null 2>&1 || true

# --- 4. Remove any lingering kernel-module route artifacts -------------------
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

# --- 5. Attach to any already-connected controller ---------------------------
# The udev rule covers future connects; attach now in case the controller is
# already paired and powered on.  (If it was connected before the rule existed,
# power-cycle it once so hid-nintendo binds with the rewritten descriptor.)
run_priv udev-hid-bpf add - "/etc/udev-hid-bpf/$OBJ" >/dev/null 2>&1 || true

log "done"