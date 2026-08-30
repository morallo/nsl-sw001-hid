#!/usr/bin/env bash
#
# Self-seeding ensure script for the N-SL SW001 HID driver on SteamOS.
#
# Persistence model:
#   - SteamOS replaces /usr (root) on every A/B update, wiping DKMS sources
#     (/usr/src) and built modules (/lib/modules/<kern>/...).
#   - /home survives updates untouched.
#   - /etc is an overlay kept in /var but only certain files survive (see
#     /etc/atomic-update.conf.d/).
#   - Therefore the ONLY reliable persistent copy of everything is under
#     /home/.steamos-nsl-sw001/.  This script re-seeds /usr and /etc from it
#     and rebuilds the module for the current kernel on every boot.
#
# The unit (steamos-nsl-ensure.service) is preserved by the atomic-update
# keep-list, and this script also re-asserts the unit + enablement, so it
# keeps running across updates.

set -euo pipefail

SEED="/home/.steamos-nsl-sw001"
SRC_DIR="/usr/src/nsl-sw001-hid-1.0"
DRIVER="hid-nsl-sw001"
PKG="nsl-sw001-hid/1.0"

log() { echo "[nsl-sw001-ensure] $*"; }

if [ ! -f "$SEED/steamos-nsl-ensure.sh" ]; then
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
        # Re-enable read-only only if it was enabled before (default).
        run_priv steamos-readonly enable || true
    fi
}

refresh_readonly
trap 'RO' EXIT

# --- 1. Install / ensure matching kernel headers -----------------------------
# SteamOS kernel packages are named linux-neptune-<majmin>-headers where majmin
# is kernel major+minor (6.16 -> 616), matching the "-neptune-616-" token in
# the uname release.  Ensure dkms + tools exist and that package is present.
KERNEL="$(uname -r)"
if ! command -v dkms >/dev/null 2>&1; then
    log "installing dkms and build dependencies"
    refresh_readonly
    MAJMIN="$(echo "$KERNEL" | sed -E 's/.*-neptune-([0-9]+)-.*/\1/')"
    if [ -z "$MAJMIN" ] || [ "$MAJMIN" = "$KERNEL" ]; then
        MAJMIN="616"
    fi
    run_priv pacman-key --init >/dev/null 2>&1 || true
    run_priv pacman-key --populate archlinux >/dev/null 2>&1 || true
    run_priv pacman-key --populate holo >/dev/null 2>&1 || true
    run_priv pacman --noconfirm -S --needed dkms base-devel gcc make \
        "linux-neptune-${MAJMIN}-headers" || true
    refresh_readonly
fi

# --- 2. Re-seed DKMS source if missing (fresh slot) --------------------------
if [ ! -d "$SRC_DIR" ]; then
    log "DKMS source missing; reseeding from $SEED/src"
    run_priv mkdir -p /usr/src
    # Copy in the current master copy from seed.
    run_priv cp -a "$SEED/src/." "$SRC_DIR/"
    run_priv chown -R root:root "$SRC_DIR"
fi

# --- 3. Build & install the module for the running kernel --------------------
if command -v dkms >/dev/null 2>&1; then
    log "building $PKG for kernel $KERNEL"
    run_priv dkms add "$SRC_DIR" >/dev/null 2>&1 || true
    run_priv dkms build -m "$(echo "$PKG" | cut -d/ -f1)" -v "$(echo "$PKG" | cut -d/ -f2)" -k "$KERNEL" || true
    run_priv dkms install -m "$(echo "$PKG" | cut -d/ -f1)" -v "$(echo "$PKG" | cut -d/ -f2)" -k "$KERNEL" || true
fi

# --- 4. Re-assert /etc config files ------------------------------------------
# modprobe blacklist
if [ ! -f /etc/modprobe.d/blacklist-hid-nintendo.conf ] || \
   ! grep -q "blacklist hid_nintendo" /etc/modprobe.d/blacklist-hid-nintendo.conf; then
    log "re-asserting modprobe blacklist"
    run_priv mkdir -p /etc/modprobe.d
    run_priv cp "$SEED/etc/blacklist-hid-nintendo.conf" /etc/modprobe.d/blacklist-hid-nintendo.conf
fi

# modules-load
if [ ! -f /etc/modules-load.d/nsl-sw001.conf ] || \
   ! grep -q "hid-nsl-sw001" /etc/modules-load.d/nsl-sw001.conf; then
    log "re-asserting modules-load conf"
    run_priv mkdir -p /etc/modules-load.d
    run_priv cp "$SEED/etc/modules-load-nsl-sw001.conf" /etc/modules-load.d/nsl-sw001.conf
fi

# atomic-update keep-list (so config + unit survive future updates)
if [ ! -f /etc/atomic-update.conf.d/nsl-sw001.conf ]; then
    log "re-asserting atomic-update keep-list"
    run_priv mkdir -p /etc/atomic-update.conf.d
    run_priv cp "$SEED/etc/atomic-update-additional-keep-list.conf" \
        /etc/atomic-update.conf.d/nsl-sw001.conf
fi

# ensure unit itself (in case of a fresh slot)
run_priv cp "$SEED/etc/steamos-nsl-ensure.service" \
    /etc/systemd/system/steamos-nsl-ensure.service
run_priv systemctl daemon-reload
run_priv systemctl enable steamos-nsl-ensure.service >/dev/null 2>&1 || true

# /etc/environment SDL vars (for Game Mode): re-append if an update dropped
# them despite the keep-list.
if ! grep -q "nsl-sw001" /etc/environment 2>/dev/null; then
    log "re-asserting SDL vars in /etc/environment"
    run_priv sh -c "cat '$SEED/etc/environment-system-append.conf' >> /etc/environment"
fi

# --- 5. Load driver ----------------------------------------------------------
log "loading $DRIVER"
run_priv modprobe "$DRIVER" || true

log "done"
