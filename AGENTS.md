# Context

I have a 3rd party controller N-SL SW001 that pretends to be a nintendo switch controller. Modes:
  - Bluetooth: compatible with real Nintendo Switch, same features as genuine. Works in Android, but no rumble.
  - USB: the controller emulates an Xbox360 controller in XInput mode. This is the only mode on PC according to the manual. No bluetooth supported. 

N-SL SW001 is impersonating a genuine Nintendo Switch Pro Controller at the USB/Bluetooth identity level (057E:2009), but it does not implement the Nintendo Pro Controller HID protocol that hid-nintendo expects. But it seems that it is "good enough" for the Switch.

I have coded a custom Linux HID driver (nsl-sw001-hid) that makes the Nintendo N-SL SW001 Bluetooth controller ("Pro Controller" clone, BD_ADDR starting with 9c:54:00) get detected and work in HID generic mode, by ignoring the presented report_descriptor.

## Hardware note

Two SW001 units in the field, same model: the originally-documented one is BD_ADDR `9c:54:00:4b:8f:a7`; a second unit is `9c:54:00:4c:c9:3b` (this is the one used by the phone and the PC captures in this session). Both impersonate 057E:2009 and report the same stock descriptor.

# Standard behavior in Linux

                    N-SL SW001

                  Bluetooth HID
                       │
                       ▼
                ┌──────────────┐
                │ HID descriptor│
                └──────┬───────┘
                       │
             ┌─────────┴─────────┐
             │                   │
       hid-nintendo         hid-generic
             │                   │
      sends Nintendo             │
      initialization             │
             │                   │
       controller doesn't        │
       understand it             │
             │                   │
          -110                   │
                                 │
                           sends nothing
                                 │
                                 ▼
                         controller doesn't
                         enter its active mode

One maybe relevant note about the behavior observed with btmon:
- In Linux using hid-generic (hid-nintendo blacklisted), when pairing the controller via bluetooth for the first time, it pairs but never starts sending the 0x30 reports. After disconnection and reconnection, it starts sending the reports
- In Android, using hid-nintendo, it works from the first time.
                         
## Output using hid_nintendo

 august 24 22:10:41 nobara-pc kernel: input: Pro Controller as /devices/virtual/misc/uhid/0005:057E:2009.000A/input/input27
 august 24 22:10:41 nobara-pc kernel: input: Pro Controller Mouse as /devices/virtual/misc/uhid/0005:057E:2009.000A/input/input28
 august 24 22:10:41 nobara-pc kernel: hid-generic 0005:057E:2009.000A: input,hidraw7: BLUETOOTH HID v0.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 august 24 22:10:41 nobara-pc kernel: nintendo 0005:057E:2009.000A: hidraw7: BLUETOOTH HID v80.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 august 24 22:10:45 nobara-pc kernel: nintendo 0005:057E:2009.000A: Failed to get joycon info; ret=-110
 august 24 22:10:45 nobara-pc kernel: nintendo 0005:057E:2009.000A: Failed to retrieve controller info; ret=-110
 august 24 22:10:45 nobara-pc kernel: nintendo 0005:057E:2009.000A: Failed to initialize controller; ret=-110
 august 24 22:10:45 nobara-pc kernel: nintendo 0005:057E:2009.000A: probe - fail = -110
 august 24 22:10:45 nobara-pc kernel: nintendo 0005:057E:2009.000A: probe with driver nintendo failed with error -110
 
 No HID device nor events detected by evtest or hid-recorder

## Output using hid-generic

output of journalctl after connecting the controller with hid-generic
 august 24 23:30:47 nobara-pc kernel: input: Pro Controller as /devices/virtual/misc/uhid/0005:057E:2009.0010/input/input35
 august 24 23:30:47 nobara-pc kernel: input: Pro Controller Mouse as /devices/virtual/misc/uhid/0005:057E:2009.0010/input/input36
 august 24 23:30:47 nobara-pc kernel: hid-generic 0005:057E:2009.0010: input,hidraw7: BLUETOOTH HID v80.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 august 24 23:31:30 nobara-pc kernel: input: Pro Controller as /devices/virtual/misc/uhid/0005:057E:2009.0011/input/input37
 august 24 23:31:30 nobara-pc kernel: input: Pro Controller Mouse as /devices/virtual/misc/uhid/0005:057E:2009.0011/input/input38
 august 24 23:31:30 nobara-pc kernel: hid-generic 0005:057E:2009.0011: input,hidraw7: BLUETOOTH HID v0.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 
 HID device present, but no events detected by evtest or hid-recorder. There is a conflict between the report_descriptor and the actual reports sent by the device.
                         
# Current project status

Custom driver in nsl-sw001-hid/hid-nsl-sw001.c makes the controller work via bluetooth with hid-nintendo excluded. All axes and buttons work. IMU data provided in a separate device similar to what Nintendo does.
Compatible with SDL only with a custom SDL_GAMECONTROLLERCONFIG. Without it, it needs a manual re-mapping through the UI.

## Working

- **Dedicated driver** (`hid-nsl-sw001.c`) with a rewritten report descriptor, `raw_event` hook, `input_mapping` and `input_configured`.
- **Y-axis direction fix (working):** the controller reports Y reversed vs. convention (down pushes raw Y toward 0 -> SDL saw down as -32768). Fixed with a `raw_event` hook that bit-flips the two 12-bit Y/Ry fields (`val = 4095 - val`) in the raw 0x30 report before hid-input parses them (raw_event runs before field extraction per drivers/hid/hid-core.c). 
- **Physical-position button mapping** (south=south, east=east): A/B mapped in the driver to BTN_EAST/BTN_SOUTH and in the SDL mapping as `a:b0,b:b1`. Expect Xbox-only games to show A/B swapped.
- **SDL/Steam Input detection** via custom gamecontrollerdb line (`nsl-sw001-controllersdb.txt`) + launch wrapper `steam-nsl-shim.sh` exporting:
  - `SDL_HIDAPI_IGNORE_DEVICES="0x057e/0x2009"` (exclude ONLY the SW001 from SDL's HIDAPI Nintendo Switch driver, which it can't speak -> `Couldn't load stick calibration`; keep HIDAPI for DS3/Xbox360/XboxOne).
  - `SDL_GAMECONTROLLERCONFIG="$(cat .../nsl-sw001-controllersdb.txt)"`.
- SDL diagnostics built against system SDL3: `enum_sdl3`, `live_sdl3` (flags `--imu-off`, `--ignore i,j,k`, `--threshold N`), `live_gamepad` (uses SOUTH/EAST/NORTH/WEST roles), `oneshot_raw`.

## IMU (accel/gyro)

- The driver creates a **separate IMU input device** ("Pro Controller IMU", second `input_dev`) with accel on `ABS_X/Y/Z` and gyro on `ABS_RX/RY/RZ`, plus `INPUT_PROP_ACCELEROMETER` and absinfo resolution set — matching hid-nintendo/hid-playstation so SDL's Linux evdev sensor backend picks it up (`SDL_GamepadHasSensor` / `SDL_GetGamepadSensorData` -> SDL_SENSOR_ACCEL/GYRO).
- **Access pitfall (fixed, confirmed working in Steam Input):** the IMU node is a separate `/dev/input/eventN` that did NOT get the `uaccess` ACL the gamepad node gets (Steam's rules only tag the joystick node), so SDL's `open()` on it failed with EACCES and `SDL_GamepadHasSensor` was always false. Fixed with a udev rule tagging the node named "Pro Controller IMU" with `uaccess`:
  - Rule file: `nsl-sw001-hid/99-nsl-sw001-imu.rules` -> installed at `/etc/udev/rules.d/99-nsl-sw001-imu.rules`.
  - Note: the rule matches only after a root `udevadm trigger /sys/class/input/eventN` or a controller reconnect (a non-root `udevadm trigger` fails with EACCES on writing `/sys/.../uevent`).
  - Why only the IMU node needs its own rule: it is `ID_INPUT_ACCELEROMETER` (not `ID_INPUT_JOYSTICK`, and not a hidraw node), so neither systemd's generic joystick uaccess line nor Steam's `057E:2009` hidraw rule tags it. Genuine Pro Controllers avoid the issue because SDL/Steam use HIDAPI over hidraw for them, not evdev.
  - Wired into the Steam Deck installer: `install-nsl-sw001-deck.sh` installs it, seeds it under `/home/.steamos-nsl-sw001/etc/`, `steamos-nsl-ensure.sh` re-asserts it at every boot, and it is added to `atomic-update-additional-keep-list.conf` so it survives SteamOS A/B updates. Verified: `SDL_GamepadHasSensor`/`SDL_GetGamepadSensorData` work and Steam Input gyro is usable.

## Rumble

- **Rumble via evdev FF implemented but does not work on SW001):** the SW001 carries genuine HD-rumble linear motors (seen to vibrate on Switch/Android). The custom driver registers `EV_FF / FF_RUMBLE` on the gamepad node via `input_ff_create_memless` (memless `data` arg carries `drvdata`, so it does not depend on `input_set_drvdata` being left intact by hid-core), with the dekuNukem freq/amp lookup tables and `encode_rumble` copied verbatim from upstream hid-nintendo. Rumble is sent as the 8-byte rumble field of output report **0x01** (rumble + subcmd, 49-byte padded, no subcmd byte), matching the SW001's requirement for full 49-byte output reports; hid-nintendo's rumble-only 0x10 report is not used. A probe subcmd 0x48 (enable vibration, data 0x01) is attempted but non-fatal if the clone ignores it. Immediate send from `play_effect` + a ~50ms periodic resend from `raw_event` (sustained only while the app re-triggers the effect, mirroring hid-nintendo; the zero-amp countdown lets motors fully stop).

## TO DO

- **Capture device without globally disabling hid-nintendo**. Options to consider:
  - Using a udev rule to unbind the device from hid-nintendo.
  - hid core
  - Spoofing the VID/PID at bluetooth HID transport

- ~~**Apply the factory calibration data**~~ **DONE (2026-09-06):** the kernel driver now reads the factory IMU calibration from SPI flash at connect and applies it in `raw_event` (gyro scale 15335, not the 13371 default). The long-standing `ret=-110` root cause was NOT the wire format: it was that the synchronous subcmd read ran inside `probe()` while `hid_device_probe()` holds `driver_input_lock`, so `__hid_input_report()`'s `down_trylock()` failed and the 0x21 reply was dropped before `raw_event`. Fixed by calling `hid_device_io_start(hdev)` right after `hid_hw_open()` (same as hid-nintendo line 2249). Two supporting rdesc changes were also required and kept: (1) declare input report `0x21` (48 Const bytes) so `hid_get_report()` routes the reply to `raw_event`; (2) output report `0x01` declared/sent at the natural 16 bytes (the Android 17 capture shows the SW001 answers SHORT subcmd reports, not the genuine 49-byte padded form). The `a2 80 02` USB handshake probe was tested and is NOT required.

- **Rumble protocol hunt** The Android capture (`btsnoop_hci_rumble.log`) turns out to be ONLY ~21 ms of connection setup on the SW001 (handle 0x0c): L2CAP signaling on cid 0x0001 + 8 host->controller reports on HID Control cid 0x0041. There is NO interrupt channel (0x43) traffic and NO `a1 30` input stream in the file — so it contains no application-session rumble at all. The 8 `a2` reports are all `a2 01 <ctr> [8 zero bytes] <trailing 10 xx 80 ...>` (e.g. `a2 01 02 00 00 00 00 00 00 00 00 10 12 80 00 00 09`, repeated ctrs), i.e. NOT rumble and NOT a standard Nintendo subcmd report — they look like SPI/calibration reads done during host init. Crucially, **Android apps do not report/detect rumble capability at all, and the only motor activity is a brief autonomous buzz on connect** (the controller's own power-on self-test / LED-motor blip, not host-requested). So there is NO Android rumble output to reverse-engineer; the earlier premise ("since rumble WORKS on Android, capture its protocol") is false. The clone's connect-buzz is self-generated and not reproducible from a host output report. Implication for driver rumble: unknown whether the SW001 accepts ANY host rumble command. If pursuing further, would need a host that exposes real rumble to the SW001 (e.g. a Switch, or SDL forcing HIDAPI rumble on hidraw) to see if any 0x01/other report ever produces motor motion; otherwise rumble stays unimplemented. 0x48 enable already known non-fatal.

# Diagnostic / reload notes

- No sudo in the interactive session: user runs privileged commands (rmmod/insmod, wipe Steam logs). Steam launched via the shim after fully quitting it.
- Controller double-identifies as "Pro Controller" + "Pro Controller Mouse" (hid-generic generic nodes) regardless of driver; a hidraw node is also present. Device indices seen: `event20`, `js0`; can disconnect/reconnect.
- Reload driver with: `sudo rmmod hid-nsl-sw001 && sudo insmod /home/morgg/dev/SW001-bluetooth/nsl-sw001-hid/hid-nsl-sw001.ko` (device disconnected first).

## Bluetooth Captures

- Linux (btmon) Bluetooth pairing process: new_pairing.btsnoop
- Linux (btmon) using upstream hid-generic driver: nsl-linux.btsnoop
- Android (Bluetooth HCI log): btsnoop_hci.log
- Android 17 (Pixel 6a, Android 17, full SW001 session incl. init subcmds, no rumble): btsnoop_hci_android17.log
- SW001 factory calibration served at SPI 0x6020 (offsets decoded): bluetooth_captures/sw001_calibration_0x6020.md

## Android (Pixel 6a) behavior with the SW001 over Bluetooth

The clone's IMU WORKS on a Pixel 6a (Android 14) over BT while it fails on the PC with `hid-nintendo`. This is NOT because the SW001 special-cases Android or declares IMU in its descriptor; it's a kernel version difference.

### Mechanism (verified over adb, session 2026-09-05)

- Pixel kernel 6.1.157 (GKI) has `CONFIG_HID_NINTENDO=y`; `nintendo` driver binds the SW001 (it impersonates 057E:2009).
- The device shows up as TWO input devices on Android, proving hid-nintendo created them:
  - `Nintendo Switch Pro Controller` (event4): sticks as `ABS_X/Y/RX/RY` range -32767..32767 (12-bit→16-bit scaling), fuzz 250 flat 500 (= hardcoded `JC_MAX_STICK_MAG 32767`, `JC_STICK_FUZZ 250`, `JC_STICK_FLAT 500`), buttons exact hid-nintendo set incl. `BTN_Z`/`BTN_MODE`, dpad as `ABS_HAT0X/Y` (0/1). NOT the 8-bit 0-255 4-axis layout hid-generic would produce from the stock descriptor.
  - `Nintendo Switch Pro Controller IMU` (event5): `ABS_X/Y/Z` accel res `4096`, `ABS_RX/RY/RZ` gyro res `14247`, `INPUT_PROP_ACCELEROMETER`, `MSC_TIMESTAMP` = hardcoded `JC_IMU_ACCEL_RES_PER_G 4096` / `JC_IMU_GYRO_RES_PER_DPS 14247`.
- Both devices share `UniqueId = 9C:54:00:4C:C9:3B` (the connected unit). Android's `/system/usr/keylayout/Vendor_057e_Product_2009.kl` maps keys, and the .kl also carries a `sensor 0x00..0x05 ACCELEROMETER/GYROSCOPE X/Y/Z` block that the framework reads from the input device.
- Difference vs PC: on kernel 6.1, `nintendo` probe is resilient — when the SW001 doesn't answer the init/SPI subcmds (the -110 timeout that hard-fails probe on the Fedora PC) it logs a warning, keeps going with factory-default calibration, and still registers gamepad+IMU input devices. The SW001 autonomously streams real `0x30` input reports (report ID 0x30) without any init handshake, so `raw_event` picks the IMU up. Rumble capability is absent because `CONFIG_NINTENDO_FF` is off AND the 0x48 enable-vibration subcmd never succeeds. Note (2026-09-05 Android 17 capture): the SW001 DID answer the 0x10 SPI read of factory calibration 0x6020 (24 bytes) on Android — decoded offsets in `bluetooth_captures/sw001_calibration_0x6020.md`.

## Stock HID report descriptor of the SW001 (captured from PC hidraw7 via reporter — firmware-fixed, same on both hosts)

Rediscovery: the SW001's stock descriptor is NOT the genuine Pro Controller descriptor. It is a generic gamepad with only:
- Report ID 0x01 (Gamepad): X/Y/Z/Rz as 8-bit 0-255 (NOT 12-bit 0-4095), Hat switch (0-7 + null), 16 buttons, 2 Consumer usages (AC_Pan/C5), 1 pad byte.
- Report ID 0x02 (Mouse): 3 buttons, X/Y/Wheel relative -127..127.
- Consumer Control collection with 8 button bits (AC_Back, AC_Home, AC_Refresh, Menu, VolUp, VolDown, Delete, F13).
- NO report ID 0x30, NO 12-bit sticks, NO IMU declared, NO subcmd 0x21/0x22 report IDs.

## Why the descriptor is irrelevant to hid-nintendo (documented in kernel source, linux master)

- `hid_parse` is called (mandatory plumbing) but the driver reads NO descriptor-derived data. hid-nintendo calls `hid_hw_start(hdev, HID_CONNECT_HIDRAW)` (hid-nintendo.c:2763), deliberately omitting `HID_CONNECT_HIDINPUT`, so hid-core never calls `hidinput_connect` to build input devices from the stock descriptor (gated in hid-core.c:2322). It allocates its own input devices with hardcoded abs params (hid-nintendo.c:2042-2088, 2110-2175) and declares `INPUT_PROP_ACCELEROMETER` itself (2173). Reports are decoded by hand in `.raw_event = nintendo_hid_event` (2917→2706), matching report ID 0x30 and parsing by byte offset, independent of HID field mapping. The only hdev fields used are transport identity: bus/vendor/product/version/name/phys, plus a `version |= 0x8000` patch (2761) so SDL2's controller DB can tell the Linux-spec mapping apart.

# Reference data

- Reddit post: https://www.reddit.com/r/linux_gaming/comments/fxwh54/using_nintendo_switch_controllers_on_linux/

- Genuine controller HID Descriptors and devide identity: https://deepwiki.com/churunfa/SwitchProControllerEsp32S3/5.2-hid-descriptors-and-device-identity
- Genuine controller Report types and processing: https://deepwiki.com/churunfa/SwitchProControllerEsp32S3/5.3-report-processing
- Genuine Controller state management and input report structure: https://deepwiki.com/churunfa/SwitchProControllerEsp32S3/5.4-controller-state-management
- Genuine Controller IMU (accelerometer, gyro) reference information: nsl_sw001_IMU.md

## Nintendo Bluetooth HID protocol

- Bluetooth HID protocol: https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md
- Bluetooth subcommands: https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_subcommands_notes.md

## Kernel Drivers

- In-tree kernel driver: https://raw.githubusercontent.com/DanielOgorchock/linux/refs/heads/ogorchock/drivers/hid/hid-nintendo.c
- DMKS driver: https://github.com/nicman23/dkms-hid-nintendo
- Specific driver for 3rd party Hori controller: https://gitlab.com/cipitaua/dkms-hid-nintendolic

## Other Userland projects

- [joycond](https://github.com/DanielOgorchock/joycond): A userspace daemon to combine joy-cons from the hid-nintendo kernel driver
- [joycond-cemuhook](https://github.com/joaorb64/joycond-cemuhook): Support for cemuhook's UDP protocol for joycond devices
