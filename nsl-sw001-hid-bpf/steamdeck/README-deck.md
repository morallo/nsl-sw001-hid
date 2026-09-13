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
- **The loader + the object live in `/home`**, which
  SteamOS never prunes. After any update everything is still there.
- **No pacman** (no keyring setup, no repo issues) — the loader is a **statically
  linked** binary (libbpf/libelf/libz/libzstd built in) made once on the dev
  machine (`make deck-bundle`). Nothing to install, no `.so` to ship, no ABI
  the Deck could mismatch.
- The only `/etc` addition is the udev rule, and it is on the atomic-update
  keep-list. All live work (attach on connect) happens in udev, so there is
  **no boot service to keep running**.

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
| `loader/` | staged bundle: statically-linked loader + `nsl-sw001.bpf.o` (`make deck-bundle`) |
| `99-hid-bpf-nsl-sw001.rules` | udev rule pointing at the `/home` loader |
| `install-nsl-sw001-bpf-deck.sh` | installer (pass `--uninstall` to remove) |
| `nsl-sw001-bpf.conf` | keeps the `/etc` udev rule across A/B updates |
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
make deck-bundle   # in hid-bpf-test/: builds the statically-linked loader + object
./steamdeck/install-nsl-sw001-bpf-deck.sh
```

Options:

- `--uninstall` — remove seed, udev rule and keep-list entry.

If the old kernel-module route (`install-nsl-sw001-deck.sh`) is detected, the
installer **fully uninstalls it**: DKMS entry + source, the `/etc` artifacts
(blacklist, modules-load, IMU uaccess rule, its atomic-update keep-list), the
ensure unit, the `/home/.steamos-nsl-sw001` seed and the SDL env vars. A clean
Deck that never had the module route installs standalone and needs nothing
extra — the "uninstall" only fires when residue is found.

The one thing a full uninstall cannot remove without a `steamos-readonly`
unlock is the compiled `.ko` for the current kernel under the read-only
`/usr/lib/modules`. It is inert (modprobe blacklist support gone, no
auto-load entry, module unloaded) and disappears at the next A/B update.

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

There is no boot service. The two things that must survive an update are both
covered by the same mechanism:

- `/home/.steamos-nsl-sw001-bpf/` holds the loader, object, the
  udev rule and the keep-list; `/home` is never pruned.
- `/etc/udev/rules.d/99-hid-bpf-nsl-sw001.rules` is on the atomic-update
  keep-list (via `/etc/atomic-update.conf.d/nsl-sw001-bpf.conf`), so it stays
  in place across A/B updates and fires on every controller connect.

The only event that mattered at boot was "controller already connected when
the system comes up" — and that is not a case that needs handling here: a
Bluetooth HID device gets a fresh `add` event on every reconnect, which is
the udev rule's trigger. The SW001 is Bluetooth-only in practice, so a
`power-cycle` of the controller is always enough to (re)attach. If an attach
ever fails transiently (e.g. an unusually early connect), the next connect
retries it; a `sudo udevadm trigger` also re-runs it.

## Requirements / caveats

- **Kernel BTF**: `/sys/kernel/btf/vmlinux` must exist (CONFIG_DEBUG_INFO_BPF).
  The Deck's neptune kernel has it; the scripts warn if not.
- **CO-RE across kernel majors**: the object is compiled against whatever kernel
  produced `vmlinux.h` at build time. CO-RE relocations handle `struct`/field
  layout differences at load, but enum values and kfunc names are not
  relocated — verify once after the first Deck boot (checks above). If the Deck
  kernel lacks a kfunc, the loader fails loudly and the controller just falls
  back to the current behaviour.
- **Build-time static libs**: `make deck-bundle` needs `glibc-static
  libbpf-static libzstd-static zlib-ng-compat-static` installed on the dev
  machine plus the `elfutils` source for `libelf.a`/`libeu.a` (built once into
  `hid-bpf-test/.build/`, cached). The resulting binary is self-contained; a
  future SteamOS glibc bump is covered by glibc forward compatibility.
- **Genuine Pro Controllers** matching `057E:2009` are left untouched by the
  program: `hid_rdesc_fixup` skips any descriptor that already declares report
  `0x21`.
- **Capture button in Steam Input** stays unbound (Steam's built-in Switch Pro
  mapping has no `misc1:`), same as the module route.