# nsl-sw001-hid-bpf: SW001 → hid-nintendo via a HID-BPF filter

**HID-BPF** filter that makes the in-tree **`hid-nintendo`** driver bind and 
work on the N-SL SW001 clone — no module, no `hid-nintendo` blacklist.

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
      (`hid_report_len` = 10). A single packet holds the motors until a
      zero-amplitude report arrives: the SW001 latches the setpoint of the last
      `0x10` it receives and **has no self-timeout** — a non-zero report with no
      zero behind it runs the motors *indefinitely*, not for a few seconds.
      Note that "zero amplitude" is a specific encoding (`0x00 0x40` per motor,
      what SDL sends as `00 01 40 40`); an all-zero rumble field is an
      undecodable packet, not a stop.

    - **A zero-amplitude report is discarded if it arrives in the same sampling
      window as the last non-zero one, and the motors latch on forever.** This
      is the important result, and it is the whole reason rumble does not work
      from Steam. The hardware samples the setpoint on a ~50 ms cadence; a zero
      that lands in the window which already carried a non-zero setpoint is
      dropped, so the controller keeps the last amplitude it accepted. Only a
      zero landing in a *later* window stops the motors. Measured, with the stop
      deliberately issued 30 ms after the last active packet (i.e. in-cadence):

      | stop sent | outcome |
      |---|---|
      | 1 zero, 30 ms after last active (in-cadence) | **fails, motors latch indefinitely** |
      | 1 zero, 500 ms after last active (out of cadence) | stops |
      | 5 zeros back-to-back, 30 ms after | stops *sometimes* |
      | 5 zeros 50 ms apart, 30 ms after | **stops every time** |

      The back-to-back case is the tell: every copy lands in the same window as
      the active packet, so repetition cannot substitute for spacing. The
      kernel driver's own rumble worker is what normally supplies that spacing,
      which is why the clone sends 5 zero reports 50 ms apart.

      **Steam sends exactly one zero per click, 30–48 ms after its last active
      packet — so Steam's stop is always mistimed and never works.** A 87 s
      btmon capture across 39 clicks / 285 rumble reports found 39 bursts, every
      one ending in a byte-perfect `00 01 40 40 00 01 40 40`, 285/285 packets
      well-formed, one distinct active payload, steady 50.0 ms spacing — and the
      motors latched anyway, because a *correct* stop in the *wrong* window is
      not a stop. The packet log alone will never show this defect; it is only
      visible as a motor that keeps turning.
   - **SDL does not end an effect when its duration elapses.** SDL's HIDAPI
     driver keeps no duration, so `SDL_JoystickRumble`'s `duration_ms` is
     dropped and only an explicit `rumble(0, 0)` stops the motors. SDL2 has no
     `SDL_GameControllerStopRumble` at all, so writing the zeros is the
      caller's job. Any game that stops asking for rumble without zeroing it
      leaves a non-genuine *or* genuine controller buzzing indefinitely, and no
      amount of translation work in this file can catch that: the failure mode
      is that no further output report ever arrives.
    - **the paced stop needs a clock, and `bpf_timer` is the one that works.**
      A host that stops asking for rumble without zeroing it leaves the motors
      running forever, so something must send zeros after the host goes quiet.
      BPF cannot sleep and `bpf_wq_start` takes no delay argument, so the wq
      callback can only be the thing that *performs* the write; the pacing has
      to come from a timer. The design is a `bpf_timer` at
      `JC_RUMBLE_PERIOD_MS` (50 ms) whose callback is non-sleepable and so only
      kicks a sleepable `bpf_wq` callback, which does the actual
      `hid_bpf_hw_output_report()`. Verified on the wire: 10 consecutive Steam
      clicks, each followed by exactly 5 zero reports at 50/50/50/50 ms, 10/10.

      **A note on `make check-kfuncs`, which is now known to be unsound for
      this:** it resolves kfuncs by *name* against vmlinux BTF, which finds
      whatever prototype the kernel publishes under that name rather than the
      `bpf_kfunc` id the program actually references. On 7.2.6-201.nobara.fc44
      it reports `bpf_timer_init` as `nargs=5 int int int int int -> STUB, not
      callable` — and the timer works anyway. Treat that verdict as advisory
      only; the load is the ground truth, and a "not callable" line is not
      evidence that a call will fail to resolve. Do not delete working timer
      code on the strength of that output.
    - **`struct_ops/hid_device_event` must not be used on this device.** It
      attaches, but calling it is a kernel NULL deref: `BUG: kernel NULL pointer
      dereference` at `dispatch_hid_bpf_device_event+0xb8`, `memcpy` with
      `RDI: 0x10` and `RCX: 0x31` (the 49-byte 0x30 report). The precondition is
      hid-nintendo's probe failing (`probe with driver nintendo failed with error
      -110`), so no valid report data exists yet HID-BPF dispatches anyway. It
      faults before our program is entered, so no program body can avoid it. The
      resulting Oops leaves bluetoothd in D state ("exited with irqs disabled")
      and needs a reboot. This is also why the controller's own `0x30` input
      stream — which arrives at 97–100 Hz and would otherwise be the natural
      clock — cannot be used: the only hook that runs on input reports is the one
      that crashes. The `bpf_timer` above is the workaround.
    - **the driver's 5 x 50 ms cascade exists, but the hidraw path bypasses it,
      so the hidraw path has no pacing at all.** hid-nintendo arms a 5-packet
      zero countdown on any non-zero amplitude
      (`JC_RUMBLE_ZERO_AMP_PKT_CNT = 5`) and drains it one packet per
      `JC_RUMBLE_PERIOD_MS = 50` ms window from a sleepable workqueue. That is
      correct, and it is what makes rumble work on `/dev/input/event21` (which
      exposes `FF_RUMBLE`). It is simply not in the code path we care about:
      Steam and SDL use hidraw, where `hid_send_report()` falls through to
      `hid_hw_output_report()` because uhid has no `send_report`, so the
      driver's rumble worker never sees the report. Measured, not inferred: in
      the 87 s capture the controller's `0x30` input reports kept arriving at
      97–100 Hz throughout and after Steam's stop — the countdown's driver
      *was* awake and being fed — yet **zero** driver-generated zero reports
      ever appeared on the wire. So on hidraw the countdown is never armed, and
      an earlier version of this file that described the driver as "already
      pacing the stop" was describing a path Steam does not take.

    - **SOLVED: the paced stop is implemented here, in BPF, with a `bpf_timer`.**
      This is the fix for the latch. When the host sends a zero-amplitude
      report, this file does not rely on that one report landing correctly —
      it arms a 50 ms timer and emits 5 zero reports of its own, one per timer
      tick, so at least one of them is guaranteed to reach a fresh sampling
      window. A generation counter invalidates a queued write if a newer
      non-zero rumble or stop supersedes it, and a stale pin is replaced on
      reconnect. The `sw001_rumble` map holding the timer/wq state is pinned
      alongside the struct_ops link by the loader, because a map containing a
      `bpf_timer` needs a userspace reference to outlive the loading process.

      Verified on the wire via btmon (`diagnostic_scripts/capture_rumble_btmon.sh`
      + `analyze_rumble_btmon.py`): 10 consecutive Steam clicks, each followed
      by exactly 5 zero reports at 50/50/50/50 ms — 10/10, zero failures. The
      trailing single "burst ending on an active report" in that capture is the
      capture being cut off mid-click, not a missed stop.

      The recovery path for an already-latched motor is still
      `python3 ../diagnostic_scripts/test_rumble.py --stop 10`, but with the
      cascade in place a latched motor should no longer be reachable through
      normal use.

      Rejected along the way, recorded so they are not retried:
      - Subcommand `0x48` (disable vibration) does **not** clear a latched
        rumble, re-tested with a decodable neutral payload so the subcommand was
        the only variable.
      - Repeating the zero back-to-back is not a substitute for spacing: 5
        back-to-back copies land in the same window as the active packet and
        stop the motor only *sometimes*. Only pacing works.
      - The controller has **no self-decay**; a non-zero report with no stop
        behind it runs the motors indefinitely.

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
