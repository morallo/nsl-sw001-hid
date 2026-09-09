# N-SL SW001 HID-BPF program on Steam Deck (SteamOS)

Deploys the HID-BPF program (`nsl-sw001.bpf.o`) on a Steam Deck. This is the
**module-free route**: instead of the `hid-nsl-sw001` DKMS module, the in-tree
`hid-nintendo` driver is made to work on the SW001 clone by a small eBPF
filter.

No build happens on the Deck — the object is prebuilt (CO-RE) and needs no
per-kernel rebuild, which is the whole point over the DKMS route.

## No pacman, no unlock, no reinstall

Unlike the first udev-hid-bpf-based packaging, this route loads the program
with a **small self-contained loader** (`sw001-bpf-attach`) instead of the
`udev-hid-bpf` pacman package. Consequences:

- **Never touches `/usr`** — nothing to unlock with `steamos-readonly`, nothing
  that dies on A/B updates.
- **The loader + its bundled .so deps + the object live in `/home`**, which
  SteamOS never prunes. After any update everything is still there.
- **No pacman** (no keyring setup, no repo issues) — the loader is a ~20 kB
  binary built once on the dev machine (`make deck-bundle`).
- The only `/etc` additions (udev rule + ensure unit) are on the
  atomic-update keep-list, and the ensure service re-asserts them every boot.

## What it gives you over the kernel-module route

| | Module route | BPF route |
|---|---|---|
| Custom module / DKMS / kernel headers | yes, rebuilt per kernel | **no** |
| `hid-nintendo` blacklisted (kills genuine Pro Controllers too) | yes | **no** |
| `SDL_HIDAPI_IGNORE_DEVICES` + custom gamecontrollerdb | yes | no (HIDAPI works: BPF fakes stick/IMU SPI cal + short 0x10 rumble re-send) |
| IMU uaccess udev rule | yes (evdev sensor node) | no (SDL reads gyro over hidraw, already tagged by Steam's rules) |
| pacman / steamos-readonly unlock | yes (loader) | **no** |

Everything else (gamepad + IMU input devices, factory IMU calibration, rumble,
`0x8000` version flag) comes from `hid-nintendo` itself once the BPF program
rewrites the report descriptor and fakes the SPI calibration reads.

## Files

| File | Purpose |
|---|---|
| `sw001-bpf-attach.c` | self-contained loader source (built off-Deck) |
| `loader/` | staged bundle: loader binary + bundled `.so`s + `nsl-sw001.bpf.o` (`make deck-bundle`) |
| `99-hid-bpf-nsl-sw001.rules` | udev rule pointing at the `/home` loader |
| `install-nsl-sw001-bpf-deck.sh` | installer (pass `--uninstall` to remove) |
| `steamos-nsl-sw001-bpf-ensure.sh` | boot-time re-assert script (re-copies `/etc` artifacts, re-attaches) |
| `steamos-nsl-sw001-bpf.service` | systemd unit that runs the ensure script at boot |
| `atomic-update-additional-keep-list.conf` | keeps the `/etc` udev rule + unit across A/B updates |
| `README-deck.md` | this file |

## How the loader works

`sw001-bpf-attach DEVPATH OBJ` (run by udev on controller connect, with
`$sys$devpath`):

1. reads the numeric HID id from the kernel device name (`0005:057E:2009.000A`
   → `0x000A`) — the `dev_set_name()` instance field;
2. patches it into the struct_ops map value (offset 0, `struct hid_bpf_ops`),
   which the kernel's attach needs to find the right device;
3. runs the object's `SEC("syscall") probe` via `BPF_PROG_TEST_RUN` (the probe
   stashes the id for the rumble-stop workqueue callback);
4. attaches the struct_ops map and **pins the link** under
   `/sys/fs/bpf/hid/<device>/` so it survives the process exiting.

It uses the exact same libbpf calls as `udev-hid-bpf` (which this replaces).

## Install

On a dev machine with clang + gcc + libbpf dev headers, build the object and
the loader bundle; then copy the `steamdeck/` tree to the Deck and run:

```bash
make deck-bundle   # in hid-bpf-test/: builds loader/ + bundles .so deps
./steamdeck/install-nsl-sw001-bpf-deck.sh
```

Options:

- `--uninstall` — remove seed, udev rule, ensure service and keep-list entry.

If the old kernel-module route (`install-nsl-sw001-deck.sh`) is detected, it is
removed automatically: its global `hid_nintendo` blacklist and
`SDL_HIDAPI_IGNORE_DEVICES` would otherwise defeat the BPF route.

After install, fully restart Steam, and **power-cycle the controller once** if
it was connected during install (the report-descriptor rewrite only applies at
device bind).

## Verify

```bash
# Program loaded for the device:
bpftool prog             # sw001_fix_rdesc / sw001_fake_spi / sw001_tick_stop
ls /sys/fs/bpf/hid/*/    # pinned links
# hid-nintendo bound it (NOT hid-generic):
ls /sys/bus/hid/devices/0005:057E:2009.*/driver
# No probe timeout:
journalctl -k -f         # unplug/replug -> "input: Pro Controller ... IMU", no -110
```

Steam Input should show the Pro Controller with working gyro and rumble (SDL
HIDAPI over hidraw).

## How persistence across A/B updates works

- `/home/.steamos-nsl-sw001-bpf/` holds the loader, bundled libs, object and a
  copy of every script; `/home` is never pruned.
- `/etc/udev/rules.d/99-hid-bpf-nsl-sw001.rules` and the ensure unit are on
  the atomic-update keep-list, so early-boot connects load the program
  immediately.
- Every boot, `steamos-nsl-sw001-bpf-ensure.service` re-asserts the `/etc`
  copies (one-time cost, pure `/etc` overlay writes — no unlock) and attaches
  to an already-connected controller.
- Companion note: the ensure script also removes a lingering module-route
  blacklist, which an update could resurrect from the module route's own
  keep-list.

## Requirements / caveats

- **Kernel BTF**: `/sys/kernel/btf/vmlinux` must exist (CONFIG_DEBUG_INFO_BPF).
  The Deck's neptune kernel has it; the scripts warn if not.
- **CO-RE across kernel majors**: the object is compiled against whatever kernel
  produced `vmlinux.h` at build time. CO-RE relocations handle `struct`/field
  layout differences at load, but enum values and kfunc names are not
  relocated — verify once after the first Deck boot (checks above). If the Deck
  kernel lacks a kfunc, the loader fails loudly and the controller just falls
  back to the current behaviour.
- **`.so` versions**: the bundled libz/libzstd/libelf/libbpf are pinned to
  whatever the dev machine shipped. They are stable across SteamOS kernels; a
  future Deck glibc bump is covered by glibc forward compatibility. Re-stage
  with `make deck-bundle` after a dev-machine update if you want to refresh.
- **Genuine Pro Controllers** matching `057E:2009` are left untouched by the
  program: `hid_rdesc_fixup` skips any descriptor that already declares report
  `0x21`.
- **Capture button in Steam Input** stays unbound (Steam's built-in Switch Pro
  mapping has no `misc1:`), same as the module route.