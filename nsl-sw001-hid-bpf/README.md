# nsl-sw001-hid-bpf: SW001 → hid-nintendo via a HID-BPF filter

Experiment: replace the custom `hid-nsl-sw001` kernel module with a **HID-BPF**
filter that makes the in-tree **`hid-nintendo`** driver bind and work on the
N-SL SW001 clone — no module, no `hid-nintendo` blacklist.

## Why this can work (verified against kernel 7.2.3 / torvalds master)

- `hid-nintendo` parses reports **0x30** (input) and **0x21** (subcmd reply) by
  byte offset in its `.raw_event`, and ignores everything else about the
  descriptor (`hid_hw_start(hdev, HID_CONNECT_HIDRAW)` only).
- But hid-core only reaches `.raw_event` for report IDs **declared in the
  device descriptor** (`__hid_input_report()` in hid-core.c). The SW001's stock
  descriptor declares neither → 0x21 subcmd replies are dropped (the probe-time
  SPI reads time out with `-110`, hid-nintendo never binds) and the 0x30 input
  stream is dropped (no input, no IMU).
- The failing kernel's `hid-nintendo.ko` already calls `hid_device_io_start()`
  during probe, so the probe-time input-lock issue is already solved upstream.
- `hid-nintendo` sends its subcmds through `hid_hw_output_report()`, which is
  exactly the path the BPF `hid_hw_output_report` struct_ops callback
  intercepts, if out-report rewriting is ever needed.

So a `hid_rdesc_fixup` that appends report-ID declarations for 0x21 + 0x30 to
the stock descriptor should be the entire missing piece.

## What the BPF program does

`nsl-sw001.bpf.c` attaches to any `0005:057E:2009` (BT Pro Controller ID):

1. **`hid_rdesc_fixup`** — appends HID items declaring input report **0x21**
   (48 Const bytes) and input report **0x30** (48 Const bytes) so hid-core
   routes both to hid-nintendo's `raw_event`. Skipped when the descriptor
   already declares 0x21 (a genuine Pro Controller connected to the same box is
   left untouched).
2. **`hid_hw_output_report`** — two jobs:
   - fakes the probe/init SPI-flash reads that the SW001 answers wrongly or
     not at all: stick user+factory calibration, the IMU user-magic check
     (answered with *no* B2 A1 magic so hid-nintendo and SDL HIDAPI take the
     factory-cal path), and the IMU factory calibration at `0x6020` (answered
     with the SW001's real values captured from the Android 17 session). Each
     faked read forwards the original output report AND injects a synthetic
     `0x21` subcmd reply; hid-nintendo's and SDL's synchronous waiters consume
     the injected reply first.
   - answers the **subcmd `0x02` (request device info)** the same way: the
     SW001 never replies to it, and SDL/Steam HIDAPI plus hid-nintendo
     classify the controller from this reply. Steam Input gates the gyro
     feature on getting a Pro Controller type, so the fake returns type `0x03`
     (Pro) and this unit's BD_ADDR.
   - **rumble for HIDAPI mode**: SDL's HIDAPI Switch driver (BT) sends the
     rumble-only output report `0x10` zero-padded to its 49-byte Bluetooth
     packet size, which the SW001 ignores (it only reacts to short reports —
     hid-nintendo's 10-byte `{0x10, packet_num, rumble_data[8]}` works). The
     hook re-sends the short form with the sleepable `hid_bpf_hw_output_report`
     kfunc and swallows the padded original by returning `hctx->size`. For this
     the hook is declared sleepable (`SEC("struct_ops.s/hid_hw_output_report")`,
     allowed by the kernel for that member) and the rdesc fixup declares 0x10
     as a 9-byte output report so the kfunc's report lookup/clamping matches
     (`hid_report_len` = 10). SDL's periodic refresh keeps re-sending while the
     effect is held, so the ~50 ms resend the SW001 needs is preserved.
3. **`probe`** (syscall) — always matches; the loader runs it before attach
    (udev-hid-bpf or `steamdeck/sw001-bpf-attach`) so it can stash the numeric
    hid id in the rumble map for the workqueue callback.

hid-nintendo then does all the rest itself: gamepad + IMU input devices,
factory IMU calibration (gyro scale 15335, from the faked `0x6020` read),
`FF_RUMBLE`, and the `0x8000` version flag SDL uses to detect the mapping.

## Build

Needs only clang/llvm for the BPF target, libbpf + gcc for the vmlinux.h
generator, and the already-installed `udev-hid-bpf` loader. **bpftool is not
required** — `vmlinux.h` is produced by `tools/gen_vmlinux_h.c`, a ~50-line
host tool that uses the system libbpf's `btf_dump` (the same machinery bpftool
uses) to dump `/sys/kernel/btf/vmlinux`:

```
sudo dnf install clang llvm libbpf-devel   # udev-hid-bpf is already installed
make deps      # checks
make           # builds nsl-sw001.bpf.o (gen_vmlinux_h -> vmlinux.h in this dir)
make inspect   # udev-hid-bpf inspect nsl-sw001.bpf.o
```

If you do have `bpftool`, its `btf dump file /sys/kernel/btf/vmlinux format c`
produces the same `vmlinux.h`.

## Test

0. Make sure the custom module is not loaded and hid-nintendo is not
   blacklisted:
   ```
   sudo rmmod hid-nsl-sw001        # if present
   lsmod | grep hid_nintendo       # must be present/resident
   ```
1. Install the loader rule:
   ```
   make install                    # = udev-hid-bpf install nsl-sw001.bpf.o
   ```
2. Power-cycle the controller (or force a re-connect). Watch:
   ```
   journalctl -k -f      # expect "input: Pro Controller ... IMU" + no -110
   ls /dev/input/event*  # gamepad + IMU nodes
   ```
   To attach without a reconnect (device already paired):
   ```
   udev-hid-bpf list-devices                     # pick the 0005:057E:2009 syspath
   sudo udev-hid-bpf add <syspath> nsl-sw001.bpf.o
   ```
3. Sanity-check with `evtest` / `live_sdl3` / `live_gamepad` from
   `diagnostic_scripts/`.

## Diagnostics if hid-nintendo still fails

- `journalctl -k` shows `nintendo ... ret=-110`: the SW001 did not answer the
  probe subcmd within hid-nintendo's 2-try window even with the descriptor
  fixed — the SW001's idle hand-off is timing-sensitive. The BPF `probe` /
  rdesc path cannot perform a blocking wait, so this would need a small driver
  (or upstream probe hardening in hid-nintendo).
- `udev-hid-bpf inspect nsl-sw001.bpf.o` shows `hid_rdesc_fixup` present: the
  object parsed correctly.

## Revert

```
make uninstall        # removes /etc/udev/rules.d/*nsl-sw001* and /etc/udev-hid-bpf/*
```

## Steam Deck (SteamOS) deployment

`steamdeck/` packages the prebuilt object for the Deck with a **self-contained
statically-linked loader** (`sw001-bpf-attach`, built off-Deck by
`make deck-bundle`) instead of the `udev-hid-bpf` pacman package: no pacman, no
`steamos-readonly` unlock, and no reinstall after A/B updates (loader + object
live under `/home`, which SteamOS never prunes).  The installer writes the udev
rule to `/etc` and adds it to the atomic-update keep-list, and seeds
`/home/.steamos-nsl-sw001-bpf/`.  There is no boot service — the udev rule
attaches on every controller (re)connect.  The installer does **not** touch an
old kernel-module route install: that route's `hid_nintendo` blacklist and
`SDL_HIDAPI_IGNORE_DEVICES` env vars would silently defeat the BPF route, so
uninstall it first (`install-nsl-sw001-deck.sh --uninstall`).  No build happens
on the Deck — see `steamdeck/README-deck.md`.

## Known limits of the BPF approach

- The **separate "Pro Controller IMU" evdev node and `FF_RUMBLE`** are created
  by hid-nintendo itself here (that's the win over the custom module) — but a
  generic HID-BPF program cannot create input devices or register force
  feedback on its own; it only rewrites the HID byte stream.
- `hid_hw_output_report` BPF callbacks rewrite the byte stream in place; the
  in-place copy cannot change the report **length**. For rumble that was a
  limitation (SDL's padded 49-byte 0x10 could not be shortened in place), so
  this hook additionally uses the sleepable `hid_bpf_hw_output_report` kfunc,
  which sends a different-length report to the device and lets the hook drop
  the original by returning the original size.