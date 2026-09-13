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
# Loader: this script does NOT touch /usr.  It uses a self-contained STATICALLY
# LINKED loader (steamdeck/sw001-bpf-attach built with `make deck-bundle`) that
# carries libbpf/libelf/libz/libzstd inside the binary, so there is no pacman
# (no keyring), no steamos-readonly unlock, no bundled .so ABI to mismatch,
# and nothing to reinstall after an A/B update.
#
# What it does (install):
#   1. Validates we are on SteamOS-ish Linux and requires sudo (or root).
#   2. Copies the loader + prebuilt object + scripts (udev rule, atomic-update
#      keep-list) under /home/.steamos-nsl-sw001-bpf/ (the seed; /home
#      persists across updates).
#   3. Writes the udev rule to /etc and adds it to the atomic-update keep-list
#      (so it survives A/B updates).  The rule triggers the loader on every
#      controller connect -- there is no boot service to maintain.
#   4. Attaches the program to any already-connected SW001 (power-cycle the
#      controller afterwards if it was connected before, so hid-nintendo binds
#      with the rewritten descriptor).
#
# Usage:
#   ./install-nsl-sw001-bpf-deck.sh [--uninstall]
#
# Notes:
#   - Requires the kernel to have CONFIG_DEBUG_INFO_BTF (the Deck kernel does:
#     /sys/kernel/btf/vmlinux).  The script only warns if it is missing.
#   - The udev rule references /home/.steamos-nsl-sw001-bpf/ directly and is
#     on the atomic-update keep-list; /home is never pruned.
#   - Genuine Pro Controllers matching 057E:2009 are left untouched by the
#     program itself (hid_rdesc_fixup skips any descriptor that already
#     declares report 0x21).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEED="/home/.steamos-nsl-sw001-bpf"
LOADER="sw001-bpf-attach"
OBJ="nsl-sw001.bpf.o"
RULE="99-hid-bpf-nsl-sw001.rules"
KEEPLIST_CONF="nsl-sw001-bpf.conf"
LOADER_DIR="$SCRIPT_DIR/loader"

UNINSTALL=0
for a in "$@"; do
    case "$a" in
        --uninstall) UNINSTALL=1 ;;
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

# --- Locate the staged bundle (built with `make deck-bundle`) ----------------
require_bundle() {
    [ -x "$LOADER_DIR/$LOADER" ] || \
        die "steamdeck/loader/$LOADER not found; build it first (make deck-bundle in hid-bpf-test/)"
    [ -f "$LOADER_DIR/$OBJ" ] || \
        die "steamdeck/loader/$OBJ not found; build it first (make deck-bundle in hid-bpf-test/)"
}

# --- Attach to any currently-connected SW001 ---------------------------------
# Deferred attaches are fine: the udev rule picks them up on the next connect.
attach_devices() {
    for d in /sys/bus/hid/devices/0005:057E:2009.*; do
        [ -d "$d" ] || continue
        if run_root "$SEED/$LOADER" "$d" "$SEED/$OBJ" >/dev/null 2>&1; then
            log "attached to $(basename "$d")"
        else
            log "attach to $(basename "$d") deferred (udev rule will load it on connect)"
        fi
    done
}

# --- Uninstall ---------------------------------------------------------------
uninstall() {
    log "uninstalling"
    run_root rm -f "/etc/udev/rules.d/$RULE"
    run_root rm -f "/etc/atomic-update.conf.d/$KEEPLIST_CONF"
    run_root udevadm control --reload >/dev/null 2>&1 || true
    run_root rm -f /sys/fs/bpf/hid/*/nsl-sw001_bpf 2>/dev/null || true
    rm -rf "$SEED"
    log "uninstalled"
    exit 0
}

if [ "$UNINSTALL" -eq 1 ]; then
    uninstall
fi

require_bundle
log "SteamOS installer for the SW001 HID-BPF program ($OBJ)"
log "bundle: $LOADER_DIR"

if [ ! -r /sys/kernel/btf/vmlinux ]; then
    log "WARNING: /sys/kernel/btf/vmlinux missing; the Deck kernel must have"
    log "         CONFIG_DEBUG_INFO_BTF for HID-BPF to load"
fi

# --- 1. Seed under /home (persists across A/B updates, never pruned) ---------
log "seeding $SEED (loader + object + rule + keep-list)"
run_root rm -rf "$SEED"
run_root mkdir -p "$SEED"
run_root cp "$LOADER_DIR/$LOADER" "$SEED/$LOADER"
run_root cp "$LOADER_DIR/$OBJ" "$SEED/$OBJ"
run_root cp "$SCRIPT_DIR/$KEEPLIST_CONF" "$SEED/$KEEPLIST_CONF"
run_root cp "$SCRIPT_DIR/$RULE" "$SEED/$RULE"
run_root chmod +x "$SEED/$LOADER" 2>/dev/null || true
run_root chown -R root:root "$SEED"

# --- 2. /etc: udev rule + keep-list ------------------------------------------
log "installing udev rule"
run_root cp "$SEED/$RULE" "/etc/udev/rules.d/$RULE"
run_root udevadm control --reload || true

log "installing atomic-update keep-list"
run_root mkdir -p /etc/atomic-update.conf.d
run_root cp "$SEED/$KEEPLIST_CONF" "/etc/atomic-update.conf.d/$KEEPLIST_CONF"

# --- 3. Attach to an already-connected controller ----------------------------
attach_devices

echo
log "done."
log "Verify:  journalctl -k -f   (unplug/replug -> 'input: Pro Controller ... IMU', no -110)"
log "         bpftool prog | grep sw001"
log "Note: if the controller was connected during install, power-cycle it once"
log "      so hid-nintendo binds with the rewritten descriptor."