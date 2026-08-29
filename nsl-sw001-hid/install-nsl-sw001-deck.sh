#!/usr/bin/env bash
#
# Install / uninstall the N-SL SW001 HID driver on SteamOS (Steam Deck).
#
# What it does (install):
#   1. Validates we are on SteamOS and requires sudo (or running as root).
#   2. Builds and installs the driver via DKMS against the exact running kernel.
#   3. Installs the global hid-nintendo blacklist + modules-load conf.
#   4. Seeds a persistent copy under /home/.steamos-nsl-sw001/ and installs
#      the ensure service + atomic-update keep-list so it survives A/B updates.
#   5. Re-enables the read-only rootfs (unless --keep-writable).
#   6. Installs the SDL environment for Steam Input (global environment.d).
#
# Usage:
#   ./install-nsl-sw001-deck.sh [--keep-writable] [--uninstall]
#
# Notes:
#   - The global hid-nintendo blacklist disables genuine Pro Controllers too.
#     That is acceptable for the current use (no genuine controllers).  See
#     README-deck.md for the per-device alternative.
#   - No IMU/gyro work is included in this step.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEED="/home/.steamos-nsl-sw001"
PKG_NAME="nsl-sw001-hid"
PKG_VERSION="1.0"
SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VERSION}"
DRIVER="hid-nsl-sw001"

KEEP_WRITABLE=0
UNINSTALL=0
for a in "$@"; do
    case "$a" in
        --keep-writable) KEEP_WRITABLE=1 ;;
        --uninstall)    UNINSTALL=1 ;;
        *) echo "unknown arg: $a" >&2; exit 1 ;;
    esac
done

log() { echo "[nsl-sw001] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# --- Privilege helper --------------------------------------------------------
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if ! sudo -n true 2>/dev/null; then
        die "sudo access required"
    fi
    SUDO="sudo"
fi
run_root() { $SUDO "$@"; }

# --- SteamOS checks ----------------------------------------------------------
if ! command -v steamos-readonly >/dev/null 2>&1; then
    die "steamos-readonly not found; this installer targets SteamOS"
fi

KERNEL="$(uname -r)"

uninstall() {
    log "uninstalling"
    if command -v dkms >/dev/null 2>&1; then
        run_root dkms remove "$PKG_NAME/$PKG_VERSION" --all >/dev/null 2>&1 || true
    fi
    run_root rm -rf "$SRC_DIR"
    run_root rm -f /etc/modprobe.d/blacklist-hid-nintendo.conf
    run_root rm -f /etc/modules-load.d/nsl-sw001.conf
    run_root rm -f /etc/systemd/system/steamos-nsl-ensure.service
    run_root systemctl daemon-reload
    run_root systemctl disable steamos-nsl-ensure.service >/dev/null 2>&1 || true
    run_root rm -f /etc/atomic-update.conf.d/nsl-sw001.conf
    run_root rmmod "$DRIVER" >/dev/null 2>&1 || true
    rm -rf "$SEED"

    USERDIR=""
    if [ -n "${SUDO_USER:-}" ]; then
        USERDIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    else
        USERDIR="$HOME"
    fi
    if [ -n "$USERDIR" ]; then
        rm -f "$USERDIR/.config/environment.d/nsl-sw001.conf"
    fi

    if [ "$KEEP_WRITABLE" -eq 0 ]; then
        run_root steamos-readonly enable >/dev/null 2>&1 || true
    fi
    log "uninstalled"
    exit 0
}

if [ "$UNINSTALL" -eq 1 ]; then
    uninstall
fi

log "SteamOS installer for $PKG_NAME $PKG_VERSION (kernel $KERNEL)"

# --- 0. Read-only off (temporary) -------------------------------------------
run_root steamos-readonly disable

# --- 1. Dependencies: dkms + build tools + matching headers ------------------
# Valve bug: the linux-neptune headers package can lag the running kernel's
# valveXX number.  Install the package matching the running kernel.
log "installing build dependencies"
P="linux-neptune-$(echo "$KERNEL" | sed -E 's/-valve[0-9]+$//')-headers"
if pacman -Qq "linux-neptune-headers" >/dev/null 2>&1; then
    P="linux-neptune-headers"
fi
run_root pacman --noconfirm -S --needed dkms base-devel gcc make "$P"

# --- 2. Stage source + dkms.conf under /usr/src ------------------------------
log "staging DKMS source"
run_root mkdir -p "$SRC_DIR"
for f in "$SCRIPT_DIR"/hid-nsl-sw001.c "$SCRIPT_DIR"/Makefile "$SCRIPT_DIR"/dkms.conf; do
    run_root cp "$f" "$SRC_DIR/" 2>/dev/null || true
done
run_root chown -R root:root "$SRC_DIR"

# --- 3. DKMS build/install ----------------------------------------------------
log "building DKMS module"
run_root dkms add "$SRC_DIR" >/dev/null
run_root dkms build -m "$PKG_NAME" -v "$PKG_VERSION" -k "$KERNEL"
run_root dkms install -m "$PKG_NAME" -v "$PKG_VERSION" -k "$KERNEL"

# --- 4. Config files ----------------------------------------------------------
log "installing modprobe blacklist + modules-load"
run_root mkdir -p /etc/modprobe.d /etc/modules-load.d
run_root cp "$SCRIPT_DIR/blacklist-hid-nintendo.conf" /etc/modprobe.d/blacklist-hid-nintendo.conf
run_root cp "$SCRIPT_DIR/modules-load-nsl-sw001.conf"  /etc/modules-load.d/nsl-sw001.conf

# --- 5. Persistent seed + ensure service (survives A/B updates) --------------
log "seeding persistent copy under $SEED"
if [ -d "$SEED" ]; then run_root rm -rf "$SEED"; fi
run_root mkdir -p "$SEED/src" "$SEED/etc"
# Current driver source (canonical) for future rebuilds:
run_root cp "$SCRIPT_DIR"/hid-nsl-sw001.c "$SCRIPT_DIR"/Makefile "$SCRIPT_DIR"/dkms.conf "$SEED/src/"
# Config/unit templates to re-assert:
run_root cp "$SCRIPT_DIR/blacklist-hid-nintendo.conf"          "$SEED/etc/"
run_root cp "$SCRIPT_DIR/modules-load-nsl-sw001.conf"           "$SEED/etc/"
run_root cp "$SCRIPT_DIR/atomic-update-additional-keep-list.conf" "$SEED/etc/"
run_root cp "$SCRIPT_DIR/steamos-nsl-ensure.service"            "$SEED/etc/"
run_root cp "$SCRIPT_DIR/steamos-nsl-ensure.sh"                 "$SEED/steamos-nsl-ensure.sh"
run_root chmod +x "$SEED/steamos-nsl-ensure.sh"
run_root chown -R root:root "$SEED"

log "installing ensure service"
run_root cp "$SEED/etc/steamos-nsl-ensure.service" /etc/systemd/system/steamos-nsl-ensure.service
run_root systemctl daemon-reload
run_root systemctl enable steamos-nsl-ensure.service

log "installing atomic-update keep-list"
run_root mkdir -p /etc/atomic-update.conf.d
run_root cp "$SCRIPT_DIR/atomic-update-additional-keep-list.conf" /etc/atomic-update.conf.d/nsl-sw001.conf

# --- 6. SDL/Steam environment (global) ----------------------------------------
USERDIR=""
if [ -n "${SUDO_USER:-}" ]; then
    USERDIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
else
    USERDIR="$HOME"
fi
log "installing SDL environment for user $USERDIR"
if [ -n "$USERDIR" ] && [ -d "$USERDIR" ]; then
    run_root mkdir -p "$USERDIR/.config/environment.d"
    run_root cp "$SCRIPT_DIR/environment-nsl-sw001.conf" "$USERDIR/.config/environment.d/nsl-sw001.conf"
    if [ -n "${SUDO_USER:-}" ]; then
        run_root chown -R "${SUDO_USER}:" "$USERDIR/.config/environment.d"
    fi
fi

# --- 7. Load now, re-enable readonly ------------------------------------------
log "loading $DRIVER"
run_root modprobe "$DRIVER" || true

if [ "$KEEP_WRITABLE" -eq 0 ]; then
    log "re-enabling read-only rootfs"
    run_root steamos-readonly enable
else
    log "leaving rootfs writable (--keep-writable)"
fi

log "done.  Also install the desktop-environment.d via the ensure/installer;"
log "the SDL vars take effect on the next login, and you should fully restart"
log "Steam before testing."
echo
log "Verify with:  ls /sys/bus/hid/drivers/$DRIVER/   and   evtest"
