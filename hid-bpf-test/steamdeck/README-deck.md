# N-SL SW001 HID-BPF program on Steam Deck (SteamOS)

Deploys the HID-BPF program from `../nsl-sw001.bpf.o` on a Steam Deck. This is
the **module-free route**: instead of the `hid-nsl-sw001` DKMS module, the
in-tree `hid-nintendo` driver is made to work on the SW001 clone by a small
eBPF filter loaded by `udev-hid-bpf`.

No build happens on the Deck — the object is prebuilt (CO-RE) and needs no
per-kernel rebuild, which is the whole point over the DKMS route.

## What it gives you over the kernel-module route

| | Module route | BPF route |
|---|---|---|
| Custom module / DKMS / kernel headers | yes, rebuilt per kernel | **no** |
| `hid-nintendo` blacklisted (kills genuine Pro Controllers too) | yes | **no** |
| `SDL_HIDAPI_IGNORE_DEVICES` + custom gamecontrollerdb | yes | no (HIDAPI works: BPF fakes stick/IMU SPI cal + short 0x10 rumble re-send) |
| IMU uaccess udev rule | yes (evdev sensor node) | no (SDL reads gyro over hidraw, already tagged by Steam's rules) |

Everything else (gamepad + IMU input devices, factory IMU calibration, rumble,
`0x8000` version flag) comes from `hid-nintendo` itself once the BPF program
rewrites the report descriptor and fakes the SPI calibration reads.

## Files

| File | Purpose |
|---|---|
| `nsl-sw001.bpf.o` | prebuilt program (build with `make ../`) |
| `install-nsl-sw001-bpf-deck.sh` | installer (pass `--uninstall` to remove) |
| `steamos-nsl-sw001-bpf-ensure.sh` | boot-time re-assert script |
| `steamos-nsl-sw001-bpf.service` | systemd unit that runs it at boot |
| `atomic-update-additional-keep-list.conf` | keeps `/etc` copies across A/B updates |
| `README-deck.md` | this file |

## Install

Build the object on a machine with clang, then copy the repo and run on the
Deck:

```bash
make          # in hid-bpf-test/
./steamdeck/install-nsl-sw001-bpf-deck.sh
```

Options:

- `--keep-writable` — leave the rootfs writable after install.
- `--uninstall` — remove the program, udev rule, ensure service, keep-list and
  seed; leaves a previously installed kernel-module route untouched.

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
# hid-nintendo bound it (NOT hid-generic):
ls /sys/bus/hid/devices/0005:057E:2009.*/driver
# No probe timeout:
journalctl -k -f         # unplug/replug -> "input: Pro Controller ... IMU", no -110
```

Steam Input should show the Pro Controller with working gyro and rumble (SDL
HIDAPI over hidraw).

## How persistence across A/B updates works

Same pattern as the module route: the installer seeds a copy under
`/home/.steamos-nsl-sw001-bpf/`, installs the ensure service and adds the
`/etc` copies (udev rule, program, service unit) to the atomic-update keep-list.
Each boot, the ensure service re-installs `udev-hid-bpf` (pacman `extra`), the
udev rule and the program from the seed, and re-attaches to an already-connected
controller.

## Requirements / caveats

- **Kernel BTF**: `/sys/kernel/btf/vmlinux` must exist (CONFIG_DEBUG_INFO_BPF).
  The Deck's neptune kernel has it; the script warns if not.
- **CO-RE across kernel majors**: the object is compiled against whatever kernel
  produced `vmlinux.h` at build time. CO-RE relocations handle `struct`/field
  layout differences at load, but enum values and kfunc names are not
  relocated — verify once after the first Deck boot (checks above). If the Deck
  kernel lacks a kfunc, `udev-hid-bpf add` fails loudly and the controller just
  falls back to the current behaviour.
- **Genuine Pro Controllers** matching `057E:2009` are left untouched by the
  program: `hid_rdesc_fixup` skips any descriptor that already declares report
  `0x21`.
- **Capture button in Steam Input** stays unbound (Steam's built-in Switch Pro
  mapping has no `misc1:`), same as the module route.
- Validated against `udev-hid-bpf` >= 2.0 (`install --force`, rule name
  `99-hid-bpf-nsl-sw001.rules`).