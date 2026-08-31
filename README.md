# N-SL SW001 Bluetooth controller driver for Linux

A custom Linux HID driver that makes the **N-SL SW001** (a cheap Nintendo
Switch "Pro Controller" clone) work over **Bluetooth on a PC / Steam Deck**.

It works out of the box on the Switch and Android, and over USB in XInput mode,
but has **no PC Bluetooth support**. The controller impersonates a genuine
Nintendo Switch Pro Controller at the USB/Bluetooth identity level
(`057e:2009`), so the in-tree `hid-nintendo` driver claims it, fails to speak
Nintendo's protocol, times out with `-110` forever, and never releases the
device.

This project ships a dedicated `hid-nsl-sw001` driver (plus SDL/Steam Input
integration) that fixes that.

## What it does

- Provides a dedicated kernel driver (`hid-nsl-sw001.c`) that:
  - **only claims N-SL SW001 units** (matches `057e:2009` **plus** the
    `9C:54:00` Bluetooth OUI), so genuine Nintendo controllers and other
    `057e:2009` clones are left to the normal drivers;
  - **rewrites the controller's report descriptor** (the vendor descriptor
    doesn't match the real reports, which is why `hid-generic` shows a device
    but zero events);
  - **fixes the reversed Y axes** (pushing down drove the raw value toward 0,
    so SDL saw down as `-32768`) by inverting the two 12-bit stick Y fields in
    `raw_event`, before hid-input parses them, so userspace gets the standard
    convention with the rest correctly centered. No SDL axis negation needed;
  - **maps every button, stick, trigger and the D-pad** to standard evdev
    codes, with A/B mapped by *physical position* (south=south, east=east, like
    the stick labels) — expect Xbox-native games to show A/B swapped;
  - **exposes the IMU** (accelerometer + gyroscope) as six extra `EV_ABS` axes
    (`ABS_MISC + 0..5`) on the gamepad node, usable through evdev or a
    userspace DSU/Cemuhook bridge (Cemu, Yuzu, Ryujinx).
- Blacklists the broken `hid-nintendo` match so the SW001 is not stolen from
  the new driver.
  ⚠️⚠️⚠️ THIS WILL PREVENT YOUR GENUINE NINTENDO CONTROLLERS FROM WORKING!!! ⚠️⚠️⚠️
- Provides SDL 3 / Steam Input integration via:
  - `SDL_HIDAPI_IGNORE_DEVICES=0x057e/0x2009` — exclude only the SW001 from
    SDL's HIDAPI Nintendo Switch driver (which it can't speak;
    `Couldn't load stick calibration`), so SDL falls back to evdev for it;
  - a custom `SDL_GAMECONTROLLERCONFIG` gamecontrollerdb entry
    (`nsl-sw001-controllersdb.txt`).
- Works on normal desktop Linux and on Steam Deck (SteamOS), including SteamOS
  **Game Mode** and survival across SteamOS A/B updates.

## Repository layout

```
├── nsl-sw001-hid/
│   ├── hid-nsl-sw001.c                  the kernel driver
│   ├── Makefile                         kernel module build
│   ├── dkms.conf                        DKMS packaging (auto-rebuild on kernel update)
│   ├── blacklist-hid-nintendo.conf      modprobe blacklist for hid_nintendo
│   ├── modules-load-nsl-sw001.conf      load hid-nsl-sw001 at boot
│   ├── nsl-sw001-controllersdb.txt      SDL gamecontrollerdb entry
│   ├── steam-nsl-shim.sh                launch Steam with the SDL vars set
│   ├── install-nsl-sw001-deck.sh        Steam Deck installer
│   ├── steamos-nsl-ensure.{sh,service}  boot-time rebuild after SteamOS updates
│   ├── environment-*.conf               SDL env vars (system + per-user)
│   ├── atomic-update-additional-keep-list.conf  SteamOS update keep-list
│   └── README-deck.md                   in-depth Steam Deck documentation
├── capdecode.py                         decode controller reports from .btsnoop captures
├── nsl_sw001_IMU.md                     IMU notes (raw format, scaling, calibration)
└── *.btsnoop                            real Bluetooth captures used to reverse the layout
```

`nsl-sw001-hid/` also contains small SDL 3 diagnostics tools (`enum_sdl3`,
`live_sdl3`, `live_gamepad`, `oneshot_raw`) and `watch_controller.py` for
testing the setup.

## Supported hardware

Any N-SL SW001 "Pro Controller" clone.

### How the driver identifies the controller

The controller impersonates a genuine Nintendo Switch Pro Controller at the
identity level, so VID/PID alone (`057e:2009`) is **not** enough to distinguish
an SW001 from genuine controllers or other clones.

To claim only SW001 units, the driver matches by VID/PID **and** by the
Bluetooth **OUI** (the first three bytes of the BD_ADDR). Every SW001 unit — no
matter how many you have or which pairing — uses the **unregistered IEEE OUI
block `9C:54:00`** (each unit gets its own MAC suffix, e.g. `9C:54:00:4B:8F:A7`
or `9C:54:00:4C:C9:3B`). Genuine Nintendo controllers use Nintendo's registered
OUIs, so they (and other `057e:2009` clones of different OUIs) are left alone
and fall through to the normal driver path.

In `nsl-sw001-probe`, the driver parses the remote MAC from `hdev->uniq` (hidp
stores the BD_ADDR there) and returns `-ENODEV` unless it starts with
`9C:54:00`, so the core then falls back to the next driver for anything that
isn't an SW001.

Requires a Linux kernel with headers for the running kernel (`gcc`, `make`).

## Installation

### Desktop Linux

#### Quick test (no reboot, not persistent)

```bash
cd nsl-sw001-hid
make                                              # needs kernel headers
# connect the controller over Bluetooth first, then:
sudo insmod ./hid-nsl-sw001.ko
evtest                                            # pick "Pro Controller"
```

To unload again: `sudo rmmod hid-nsl-sw001` (disconnect the controller first).

#### Persistent install (DKMS + boot autoload)

```bash
cd nsl-sw001-hid

# 1. Stage the source for DKMS
sudo mkdir -p /usr/src/nsl-sw001-hid-1.0
sudo cp hid-nsl-sw001.c Makefile dkms.conf /usr/src/nsl-sw001-hid-1.0/

# 2. Build and install via DKMS (rebuilds automatically on kernel updates)
sudo dkms add -m nsl-sw001-hid -v 1.0
sudo dkms build -m nsl-sw001-hid -v 1.0
sudo dkms install -m nsl-sw001-hid -v 1.0

# 3. Keep hid-nintendo away from the SW001, and load our driver at boot
sudo cp blacklist-hid-nintendo.conf /etc/modprobe.d/
sudo cp modules-load-nsl-sw001.conf  /etc/modules-load.d/

# 4. Load it now (or reboot)
sudo modprobe hid-nsl-sw001
```

The device will auto-load the driver and expose a "Pro Controller" input
device. Note it also creates a harmless "Pro Controller Mouse" generic node
(this happens regardless of driver — the controller exposes a mouse
collection).

#### SDL / Steam Input on the desktop

Steam needs two environment variables. Either launch Steam through the shim:

```bash
./steam-nsl-shim.sh
```

or add them to your session (they are identical in
`environment-nsl-sw001.conf`):

```
SDL_HIDAPI_IGNORE_DEVICES=0x057e/0x2009
SDL_GAMECONTROLLERCONFIG=<contents of nsl-sw001-controllersdb.txt>
```

A convenient way is to copy the shipped file as a per-user environment snippet:

```bash
mkdir -p ~/.config/environment.d
cp environment-nsl-sw001.conf ~/.config/environment.d/nsl-sw001.conf
```

then log out/in and fully restart Steam.

### Steam Deck (SteamOS)

```bash
cd nsl-sw001-hid
./install-nsl-sw001-deck.sh
```

What the installer does (details in `README-deck.md`):

- Builds and installs the driver via **DKMS** against the exact running kernel.
- **Globally blacklists `hid_nintendo`** so it stops claiming the SW001.
- Writes the SDL vars into **`/etc/environment`** so they reach Steam in **both
  Game Mode and Desktop Mode** (Game Mode ignores `~/.config/environment.d/`).
- Seeds a persistent copy under `/home/.steamos-nsl-sw001/` and installs a
  `steamos-nsl-ensure` service + atomic-update keep-list, so the driver stays
  installed and working across SteamOS A/B updates (same self-healing pattern
  as Valve's NVIDIA installer).
- Re-enables the read-only rootfs after install.

Options: `--keep-writable` (leave rootfs writable), `--uninstall` (remove
everything). After install, fully restart Steam (or log out/in).

## Verify

```bash
# Controller bound by our driver (not hid-nintendo/hid-generic):
ls /sys/bus/hid/drivers/hid-nsl-sw001/
# Module loaded:
lsmod | grep hid_nsl
# BD_ADDR of the bound device (your controller MAC):
cat /sys/bus/hid/devices/0005:057E:2009.*/uniq
# Live button/axis test:
evtest
# SDL-level test (SDL3 required):
./nsl-sw001-hid/live_sdl3
```

## Known limitations

- **Global `hid_nintendo` blacklist** also disables genuine Nintendo Switch
  controllers (they'd use the broken-by-this-setup path). Fine for a setup with
  only the SW001; a per-device udev steer rule is a planned alternative.
- **`SDL_HIDAPI_IGNORE_DEVICES` is strictly VID/PID based** (`057e/0x2009`), so
  a genuine Pro Controller on the same machine also falls back to evdev and
  loses its HIDAPI gyro/rumble.
- **IMU speed/sensors:** the accelerometer and gyroscope are exposed as
  `ABS_MISC+0..5` on the main gamepad node (shown in `evtest`). SDL's Linux
  evdev sensor backend does **not** read those axes, so **Steam Input / SDL do
  not currently surface gyro/accel** — use the axes directly (evdev) or a
  userspace DSU/Cemuhook bridge (Cemu, Yuzu, Ryujinx). See `nsl_sw001_IMU.md`.
- **A/B physical mapping:** because A/B are mapped to their physical positions
  (south/east), Xbox-native games display them swapped.
- **Steam Deck Game Mode D-pad:** the D-pad fires correctly at the evdev level,
  but Steam Input may not map it automatically from the controllerdb entry on
  SteamOS SDL 3; a manual Steam Input remap works.

## How the layout was derived

The controller's real Bluetooth HID input report (`0x30`, 49 bytes) was
recovered by decoding Bluetooth captures (`capdecode.py`) while pressing each
control one at a time. The report packs the sticks as four JoyCon-style 12-bit
values and carries three 12-byte IMU samples (accel XYZ + gyro XYZ, int16 LE).
See the header comment in `hid-nsl-sw001.c` for the full byte map.

## License

MIT License. Provided as-is, no warranty. Hardware is a third-party clone; this is
an independent reverse-engineering effort, not affiliated with Nintendo.
