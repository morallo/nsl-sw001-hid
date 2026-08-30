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
    run_root sed -i '/# N-SL SW001 (added by install-nsl-sw001-deck.sh/,/^# end nsl-sw001/d' \
        /etc/environment 2>/dev/null || true
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
# SteamOS kernel packages are named linux-neptune-<majmin>-... where <majmin>
# is the kernel major+minor (e.g. 6.16 -> 616).  The uname release is
# 6.16.12-valve24.5-1-neptune-616-g<githash>; the headers package matching the
# running kernel is linux-neptune-616-headers.  Valve bug: this package can lag
# the running kernel's valveXX number, so always target the one for the running
# kernel.
log "installing build dependencies"
# Extract the "-neptune-<majmin>-" token from the uname release.
MAJMIN="$(echo "$KERNEL" | sed -E 's/.*-neptune-([0-9]+)-.*/\1/')"
if [ -z "$MAJMIN" ] || [ "$MAJMIN" = "$KERNEL" ]; then
    die "could not derive SteamOS header package from kernel release '$KERNEL'"
fi
P="linux-neptune-${MAJMIN}-headers"

# A fresh SteamOS install may not have an initialized pacman keyring; without
# it pacman fails to verify packages ("keyring is not writable").  Initialize
# and populate both Arch and Valve (holo) trust anchors first.
log "initializing pacman keyring"
run_root pacman-key --init >/dev/null 2>&1 || true
run_root pacman-key --populate archlinux >/dev/null 2>&1 || true
run_root pacman-key --populate holo >/dev/null 2>&1 || true

log "installing headers package: $P"
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
# Idempotent: clear any previous DKMS entry (source may have been replaced).
run_root dkms remove "$PKG_NAME/$PKG_VERSION" --all >/dev/null 2>&1 || true
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
run_root cp "$SCRIPT_DIR/environment-system-append.conf"         "$SEED/etc/"
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

# --- 6. SDL/Steam environment ------------------------------------------------
# SteamOS Game Mode does NOT apply ~/.config/environment.d to the Steam
# session, so Desktop Mode works but Game Mode doesn't.  The vars must be
# system-wide in /etc/environment (sourced by PAM for both Game Mode and
# Desktop Mode sessions).  Append idempotently using a marker.
ENV_FRAG="$SCRIPT_DIR/environment-system-append.conf"
log "appending SDL vars to /etc/environment (reaches Game Mode too)"
if ! grep -q "nsl-sw001" /etc/environment 2>/dev/null; then
    run_root cp /etc/environment /etc/environment.bak-nsl-sw001
    run_root sh -c "cat '$ENV_FRAG' >> /etc/environment"
else
    log "/etc/environment already contains nsl-sw001 entries; updating in place"
    run_root sed -i '/# N-SL SW001 (added by install-nsl-sw001-deck.sh/,/^# end nsl-sw001/d' /etc/environment
    run_root sh -c "cat '$ENV_FRAG' >> /etc/environment"
fi

# Keep the per-user environment.d too (harmless; covers Desktop Mode even
# without the login-time /etc/environment source, and documents the values).
USERDIR=""
if [ -n "${SUDO_USER:-}" ]; then
    USERDIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
else
    USERDIR="$HOME"
fi
if [ -n "$USERDIR" ] && [ -d "$USERDIR" ]; then
    log "also writing per-user environment.d for $USERDIR"
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
