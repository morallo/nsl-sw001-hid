#!/usr/bin/env bash
#
# Install / uninstall the N-SL SW001 HID-BPF program on SteamOS (Steam Deck).
#
# The BPF program (hid-bpf-test/nsl-sw001.bpf.o) makes the in-tree hid-nintendo
# driver bind and work on the SW001 clone -- no custom kernel module, no DKMS,
# no hid-nintendo blacklist.  It also fixes SDL's HIDAPI Switch driver (fakes
# the stick/IMU SPI calibration reads and re-sends the short 0x10 rumble form),
# so the module route's SDL hacks (SDL_HIDAPI_IGNORE_DEVICES, custom
# gamecontrollerdb, IMU uaccess rule) are not needed either.
#
# What it does (install):
#   1. Validates we are on SteamOS and requires sudo (or running as root).
#   2. Installs the udev-hid-bpf loader (pacman, extra repo).
#   3. Installs nsl-sw001.bpf.o via `udev-hid-bpf install` (udev rule + object
#      under /etc).
#   4. Seeds a persistent copy under /home/.steamos-nsl-sw001-bpf/ and installs
#      the ensure service + atomic-update keep-list so it survives A/B updates.
#   5. If the old kernel-module route (install-nsl-sw001-deck.sh) is installed,
#      removes it: its global hid-nintendo blacklist would otherwise block the
#      BPF route, and its SDL_HIDAPI_IGNORE_DEVICES would disable the HIDAPI
#      path the BPF now fixes.
#   6. Attaches the program to any already-connected SW001 (power-cycle the
#      controller afterwards if it was connected before, so hid-nintendo binds
#      with the rewritten descriptor).
#   7. Re-enables the read-only rootfs (unless --keep-writable).
#
# Usage:
#   ./install-nsl-sw001-bpf-deck.sh [--keep-writable] [--uninstall]
#
# Notes:
#   - The BPF object must already be built (run `make` in hid-bpf-test/): the
#     loader prefers a prebuilt object and the Deck has no need for clang.
#     Looked up next to this script or one directory up.
#   - Requires the kernel to have CONFIG_DEBUG_INFO_BTF (the Deck kernel does:
#     /sys/kernel/btf/vmlinux).  The script only warns if it is missing.
#   - Genuine Pro Controllers matching 057E:2009 are left untouched by the
#     program itself (hid_rdesc_fixup skips any descriptor that already
#     declares report 0x21).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEED="/home/.steamos-nsl-sw001-bpf"
OBJ="nsl-sw001.bpf.o"
RULE="99-hid-bpf-nsl-sw001.rules"
SVC="steamos-nsl-sw001-bpf.service"
KEEPLIST_CONF="nsl-sw001-bpf.conf"
ENSURE_SH="steamos-nsl-sw001-bpf-ensure.sh"

KEEP_WRITABLE=0
UNINSTALL=0
for a in "$@"; do
    case "$a" in
        --keep-writable) KEEP_WRITABLE=1 ;;
        --uninstall)    UNINSTALL=1 ;;
        *) echo "unknown arg: $a" >&2; exit 1 ;;
    esac
done

log() { echo "[nsl-sw001-bpf] $*"; }
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

# --- Locate the prebuilt BPF object ------------------------------------------
BPF_SRC=""
if [ -f "$SCRIPT_DIR/nsl-sw001.bpf.o" ]; then
    BPF_SRC="$SCRIPT_DIR/nsl-sw001.bpf.o"
elif [ -f "$SCRIPT_DIR/../nsl-sw001.bpf.o" ]; then
    BPF_SRC="$SCRIPT_DIR/../nsl-sw001.bpf.o"
fi
require_obj() {
    if [ -z "$BPF_SRC" ]; then
        die "nsl-sw001.bpf.o not found next to this script; build it first (make in hid-bpf-test/)"
    fi
}

# --- Loader ------------------------------------------------------------------
init_keyring() {
    # A fresh SteamOS install may not have an initialized pacman keyring.
    run_root pacman-key --init >/dev/null 2>&1 || true
    run_root pacman-key --populate archlinux >/dev/null 2>&1 || true
    run_root pacman-key --populate holo >/dev/null 2>&1 || true
}

ensure_loader() {
    if command -v udev-hid-bpf >/dev/null 2>&1; then
        return
    fi
    log "installing udev-hid-bpf loader"
    init_keyring
    run_root pacman --noconfirm -S --needed udev-hid-bpf
    command -v udev-hid-bpf >/dev/null 2>&1 || die "udev-hid-bpf not available; add the Arch extra repo or install it manually"
}

# --- Old kernel-module route (install-nsl-sw001-deck.sh), if present ---------
# Its global hid-nintendo blacklist and SDL_HIDAPI_IGNORE_DEVICES break the
# BPF route, so drop the whole module footprint.
MODULE_UNIT="steamos-nsl-ensure.service"
MODULE_SEED="/home/.steamos-nsl-sw001"
MODULE_DKMS="nsl-sw001-hid/1.0"
MODULE_SRC="/usr/src/nsl-sw001-hid-1.0"

purge_module_route() {
    local found=0
    [ -f /etc/modprobe.d/blacklist-hid-nintendo.conf ] && found=1
    [ -f /etc/modules-load.d/nsl-sw001.conf ] && found=1
    [ -f /etc/udev/rules.d/99-nsl-sw001-imu.rules ] && found=1
    [ -f /etc/atomic-update.conf.d/nsl-sw001.conf ] && found=1
    [ -f "/etc/systemd/system/$MODULE_UNIT" ] && found=1
    [ -d "$MODULE_SEED" ] && found=1
    [ "$found" -eq 0 ] && return

    log "removing old kernel-module route (would block the BPF route)"
    run_root rm -f /etc/modprobe.d/blacklist-hid-nintendo.conf
    run_root rm -f /etc/modules-load.d/nsl-sw001.conf
    run_root rm -f /etc/udev/rules.d/99-nsl-sw001-imu.rules
    run_root rm -f /etc/atomic-update.conf.d/nsl-sw001.conf
    run_root rm -f "/etc/systemd/system/$MODULE_UNIT"
    run_root systemctl disable "$MODULE_UNIT" >/dev/null 2>&1 || true
    if command -v dkms >/dev/null 2>&1; then
        run_root dkms remove "$MODULE_DKMS" --all >/dev/null 2>&1 || true
    fi
    run_root rm -rf "$MODULE_SRC"
    run_root rmmod hid-nsl-sw001 >/dev/null 2>&1 || true
    run_root sed -i '/# N-SL SW001 (added by install-nsl-sw001-deck.sh/,/^# end nsl-sw001/d' \
        /etc/environment 2>/dev/null || true
    run_root rm -rf "$MODULE_SEED"
    # per-user environment.d copy
    local USERDIR=""
    if [ -n "${SUDO_USER:-}" ]; then
        USERDIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    else
        USERDIR="$HOME"
    fi
    if [ -n "$USERDIR" ] && [ -f "$USERDIR/.config/environment.d/nsl-sw001.conf" ]; then
        rm -f "$USERDIR/.config/environment.d/nsl-sw001.conf"
    fi
}

# --- Uninstall ---------------------------------------------------------------
uninstall() {
    log "uninstalling"
    run_root rm -f "/etc/udev/rules.d/$RULE"
    run_root rm -f "/etc/udev-hid-bpf/$OBJ"
    run_root rm -f "/etc/atomic-update.conf.d/$KEEPLIST_CONF"
    run_root rm -f "/etc/systemd/system/$SVC"
    run_root systemctl disable "$SVC" >/dev/null 2>&1 || true
    run_root systemctl daemon-reload
    run_root udevadm control --reload >/dev/null 2>&1 || true
    rm -rf "$SEED"
    if [ "$KEEP_WRITABLE" -eq 0 ]; then
        run_root steamos-readonly enable >/dev/null 2>&1 || true
    fi
    log "uninstalled"
    exit 0
}

if [ "$UNINSTALL" -eq 1 ]; then
    uninstall
fi

require_obj
log "SteamOS installer for the SW001 HID-BPF program ($(basename "$BPF_SRC"))"
log "BPF object: $BPF_SRC"

# --- 0. Read-only off (temporary) -------------------------------------------
run_root steamos-readonly disable

# --- 1. Loader ---------------------------------------------------------------
ensure_loader
if [ ! -r /sys/kernel/btf/vmlinux ]; then
    log "WARNING: /sys/kernel/btf/vmlinux missing; the Deck kernel must have"
    log "         CONFIG_DEBUG_INFO_BTF for HID-BPF to load"
fi

# --- 2. Install the BPF program via udev-hid-bpf -----------------------------
log "installing $OBJ via udev-hid-bpf"
run_root udev-hid-bpf install --force "$BPF_SRC"
test -f "/etc/udev/rules.d/$RULE" || {
    run_root udev-hid-bpf inspect "$BPF_SRC" || true
    die "$RULE was not generated; inspect output above"
}
run_root udevadm control --reload || true

# --- 3. Persistent seed + ensure service (survives A/B updates) --------------
log "seeding persistent copy under $SEED"
run_root rm -rf "$SEED"
run_root mkdir -p "$SEED/src" "$SEED/etc"
run_root cp "$BPF_SRC" "$SEED/src/$OBJ"
run_root cp "$SCRIPT_DIR/$KEEPLIST_CONF" "$SEED/etc/"
run_root cp "$SCRIPT_DIR/$SVC" "$SEED/etc/"
run_root cp "$SCRIPT_DIR/$ENSURE_SH" "$SEED/$ENSURE_SH"
run_root chmod +x "$SEED/$ENSURE_SH"
run_root chown -R root:root "$SEED"

log "installing ensure service"
run_root cp "$SEED/etc/$SVC" "/etc/systemd/system/$SVC"
run_root systemctl daemon-reload
run_root systemctl enable "$SVC" >/dev/null

log "installing atomic-update keep-list"
run_root mkdir -p /etc/atomic-update.conf.d
run_root cp "$SEED/etc/$KEEPLIST_CONF" "/etc/atomic-update.conf.d/$KEEPLIST_CONF"

# --- 4. Drop the old kernel-module route -------------------------------------
purge_module_route

# --- 5. Attach to an already-connected controller, restore readonly ----------
log "attaching to any present SW001 (power-cycle it if it was already connected)"
run_root udev-hid-bpf add - "/etc/udev-hid-bpf/$OBJ" >/dev/null 2>&1 || \
    log "(no controller present right now; the udev rule will load it on connect)"

if [ "$KEEP_WRITABLE" -eq 0 ]; then
    log "re-enabling read-only rootfs"
    run_root steamos-readonly enable
else
    log "leaving rootfs writable (--keep-writable)"
fi

echo
log "done."
log "Verify:  journalctl -k -f   (unplug/replug -> 'input: Pro Controller ... IMU', no -110)"
log "         bpftool prog | grep sw001"