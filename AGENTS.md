# Context

I have a 3rd party controller N-SL SW001 that pretends to be a nintendo switch controller.
It connects through bluetooth to the Switch, and also to Android, and it works fine.
But it does not work over bluetooth in a PC by default, only through USB in XInput mode. No PC over bluetooth is supported according to the manual.

N-SL SW001 is impersonating a genuine Nintendo Switch Pro Controller at the USB/Bluetooth identity level (057E:2009), but it does not implement the Nintendo Pro Controller HID protocol that hid-nintendo expects.

I have coded a custom Linux HID driver (nsl-sw001-hid) that makes the Nintendo N-SL SW001 Bluetooth controller ("Pro Controller" clone, BD_ADDR 9c:54:00:4b:8f:a7) get detected and work in HID generic mode.

## Information about the IMU data in the genuine controller and upstream hid-nintendo

Only include it in the context if the user asks about the IMU, accelerometer, gyro.

Details in the file nsl_sw001_IMU.md


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
                         
# Output using hid_nintendo

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

# Output using hid-generic

output of journalctl after connecting the controller with hid-generic
 august 24 23:30:47 nobara-pc kernel: input: Pro Controller as /devices/virtual/misc/uhid/0005:057E:2009.0010/input/input35
 august 24 23:30:47 nobara-pc kernel: input: Pro Controller Mouse as /devices/virtual/misc/uhid/0005:057E:2009.0010/input/input36
 august 24 23:30:47 nobara-pc kernel: hid-generic 0005:057E:2009.0010: input,hidraw7: BLUETOOTH HID v80.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 august 24 23:31:30 nobara-pc kernel: input: Pro Controller as /devices/virtual/misc/uhid/0005:057E:2009.0011/input/input37
 august 24 23:31:30 nobara-pc kernel: input: Pro Controller Mouse as /devices/virtual/misc/uhid/0005:057E:2009.0011/input/input38
 august 24 23:31:30 nobara-pc kernel: hid-generic 0005:057E:2009.0011: input,hidraw7: BLUETOOTH HID v0.01 Gamepad [Pro Controller] on 00:41:0e:2c:0f:64
 
 HID device present, but no events detected by evtest or hid-recorder. There is a conflict between the report_descriptor and the actual reports sent by the device.
                         
# Bluetooth Captures

- Bluetooth pairing process: new_pairing.btsnoop
- Linux (btmon) using upstream hid-generic driver: nsl-linux.btsnoop
- Android: btsnoop_hci.log

# Current project status

Custom driver in nsl-sw001-hid/hid-nsl-sw001.c makes the controller work via bluetooth with hid-nintendo excluded. All axes and buttons work.

## Working

- **Dedicated driver** (`hid-nsl-sw001.c`) with a rewritten report descriptor, `raw_event` hook, `input_mapping` and `input_configured`.
- **Y-axis direction fix (working):** the controller reports Y reversed vs. convention (down pushes raw Y toward 0 -> SDL saw down as -32768). Fixed with a `raw_event` hook that bit-flips the two 12-bit Y/Ry fields (`val = 4095 - val`) in the raw 0x30 report before hid-input parses them (raw_event runs before field extraction per drivers/hid/hid-core.c). Verified against real captured reports: rest stays centered ~2048, standard direction now (down=+, up=-). This does NOT rely on SDL axis negation, avoiding the rest-offset bug.
- **Physical-position button mapping** (south=south, east=east): A/B mapped in the driver to BTN_EAST/BTN_SOUTH and in the SDL mapping as `a:b0,b:b1`. Expect Xbox-only games to show A/B swapped.
- **SDL/Steam Input detection** via custom gamecontrollerdb line (`nsl-sw001-controllersdb.txt`) + launch wrapper `steam-nsl-shim.sh` exporting:
  - `SDL_HIDAPI_IGNORE_DEVICES="0x057e/0x2009"` (exclude ONLY the SW001 from SDL's HIDAPI Nintendo Switch driver, which it can't speak -> `Couldn't load stick calibration`; keep HIDAPI for DS3/Xbox360/XboxOne).
  - `SDL_GAMECONTROLLERCONFIG="$(cat .../nsl-sw001-controllersdb.txt)"`.
- SDL diagnostics built against system SDL3: `enum_sdl3`, `live_sdl3` (flags `--imu-off`, `--ignore i,j,k`, `--threshold N`), `live_gamepad` (uses SOUTH/EAST/NORTH/WEST roles), `oneshot_raw`.

## IMU (accel/gyro)

- Currently exposed as **6 extra EV_ABS axes `ABS_MISC + 0..5`** (newest 12-byte IMU sample: accel XYZ then gyro XYZ) on the same gamepad input node; `input_configured` clears the fuzz/flat so full 16-bit samples reach userspace. Visible in `evtest`, usable by a userspace DSU/Cemuhook UDP bridge.
- **Known limitation:** SDL's Linux evdev sensor backend does NOT read `ABS_MISC` and does NOT read IMU off the main gamepad node. It reads a **separate** IMU input device with accel on `ABS_X/Y/Z` and gyro on `ABS_RX/RY/RZ` (exactly what upstream hid-nintendo / hid-playstation create, e.g. "Nintendo Switch Pro Controller IMU"). So as-is, **SDL and Steam Input will not expose gyro/accel**. An alternate exposure is needed (see TO DO).

## TO DO

- **Expose IMU to SDL/Steam Input.** Options under discussion:
  - (A) Separate IMU evdev device (recommended/standard): second `input_dev` with accel as ABS_X/Y/Z and gyro as ABS_RX/RY/RZ, matching hid-nintendo/hid-playstation. SDL then surfaces SDL_SENSOR_ACCEL/GYRO (SDL_GamepadHasSensor / SDL_GetGamepadSensorData) and Steam Input can pick it up. Set absinfo resolution for correct scaling.
  - (B) Keep ABS_MISC + userspace DSU/Cemuhook UDP bridge: good for Cemu/Yuzu/Ryujinx gyro; Steam Input won't consume it.
- jstest-gtk: axes/buttons ideally follow evdev joystick conventions (BTN_A/B etc., ABS_X) so they display as labeled sticks/buttons rather than generic (partly addressed by A/B mapping).
- **D-pad mapping in Steam Input (Deck):** D-pad fires correctly at evdev (`BTN_DPAD_*` on the gamepad node) but Steam Input doesn't map it from the `nsl-sw001-controllersdb.txt` line's `dpup:b14,dpdown:b15,dpleft:b16,dpright:b17`. Manual Steam Input remap works. Likely cause: SDL button indices for the SW001 on SteamOS SDL3 don't line up with `b14..b17`. To make it work without manual remap, determine SDL's actual button indices for the D-pad on the Deck and update the controllerdb entry.
- **SteamOS Game Mode vs Desktop Mode (Deck):** controller works in Desktop Mode but NOT in SteamOS Game Mode. Game Mode does not appear to apply `~/.config/environment.d/nsl-sw001.conf` (the `SDL_HIDAPI_IGNORE_DEVICES` / `SDL_GAMECONTROLLERCONFIG` vars don't reach the Steam process in Game Mode), so Steam Input in Game Mode falls back to its HIDAPI Nintendo driver which the SW001 can't speak. Needs a mechanism to export the SDL vars into the Game Mode Steam session (e.g. a Steam Deck session-level env, Decky startup hook, or `/etc/environment`).

## Diagnostic / reload notes

- No sudo in the interactive session: user runs privileged commands (rmmod/insmod, wipe Steam logs). Steam launched via the shim after fully quitting it.
- Controller double-identifies as "Pro Controller" + "Pro Controller Mouse" (hid-generic generic nodes) regardless of driver; a hidraw node is also present. Device indices seen: `event20`, `js0`; can disconnect/reconnect.
- Reload driver with: `sudo rmmod hid-nsl-sw001 && sudo insmod .../hid-nsl-sw001.ko` (device disconnected first).
- Bluetooth capture files: see "Bluetooth Captures" above.

# Reference data about Nintendo Bluetooth HID protocol

https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md
https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_subcommands_notes.md


