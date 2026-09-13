# N-SL SW001 Bluetooth controller driver for Linux

<img width="800" src="https://github.com/user-attachments/assets/17c8ee60-90c9-4041-a7b1-7a4b8a1dc0d4" />

A project that makes the **N-SL SW001** (a cheap Nintendo Switch "Pro
Controller" clone) work over **Bluetooth on a PC / Steam Deck**.

The controller works out of the box on the Switch and Android 17, and on a PC
over USB in Xinput mode, but has **no PC Bluetooth support**. The controller
impersonates a genuine Nintendo Switch Pro Controller at the USB/Bluetooth
identity level (VID:PID=`057e:2009`), so the in-tree `hid-nintendo` driver
claims it. But the device fails to speak Nintendo's protocol, times out with
`-110` forever, and it is never released for a working driver to claim it.

This project contains 2 different implementations to make these devices work
through bluetooth in Linux (you can use any one of them):

1. **BPF filter** — [`nsl-sw001-hid-bpf/`](nsl-sw001-hid-bpf/README.md): a
   HID-BPF program that modifies the input and output reports between the host
   and the device on-the-fly, making the in-tree `hid-nintendo` driver treat it
   as a genuine Switch Pro Controller. No module, no `hid_nintendo` blacklist,
   no SDL hacks.
2. **Kernel driver** — [`nsl-sw001-hid/`](nsl-sw001-hid/README.md): a dedicated
   custom `hid-nsl-sw001` driver (plus SDL/Steam Input integration) that parses
   the controller's input reports into standard `evdev` events and sends the
   expected output reports for the rumble effects.

## Supported hardware

Any N-SL SW001 "Pro Controller" clone with a MAC starting with `9C:54:00`.

This has only been tested with 2 controllers, but Aliexpress is full of
look-alikes that are probably the same. If you identify a similar controller
not working, please create a github issue.

Because the controller impersonates a genuine Pro Controller
(VID:PID=`057e:2009`), each implementation has its own way of telling SW001s
apart from genuine controllers — see each README (the kernel driver additionally
matches the unregistered Bluetooth OUI `9C:54:00`).

## Repository layout

```
├── nsl-sw001-hid/            kernel driver + SDL/Steam integration (→ README.md)
├── nsl-sw001-hid-bpf/        HID-BPF filter, incl. Steam Deck deployment (→ README.md)
├── capdecode.py              decode controller reports from .btsnoop captures
├── bluetooth_captures/       real Bluetooth captures used to reverse the layout
├── nsl_sw001_IMU.md          IMU notes (raw format, scaling, calibration)
```

## How the input layout was derived

The controller's real Bluetooth HID input report (`0x30`, 49 bytes) was
recovered by decoding Bluetooth captures (`capdecode.py`) while pressing each
control one at a time. The report packs the sticks as four JoyCon-style 12-bit
values and carries three 12-byte IMU samples (accel XYZ + gyro XYZ, int16 LE).
See the header comment in `nsl-sw001-hid/hid-nsl-sw001.c` for the full byte map.

## License

MIT License. See `LICENSE`. Provided as-is, no warranty. Hardware is a
third-party clone; this is an independent reverse-engineering effort, not
affiliated with Nintendo.

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

- In-tree kernel driver (hid-nintendo): https://raw.githubusercontent.com/DanielOgorchock/linux/refs/heads/ogorchock/drivers/hid/hid-nintendo.c
- Generic Pro Controller support in hid-nintendo: https://github.com/DanielOgorchock/linux/issues/10
- DMKS driver: https://github.com/nicman23/dkms-hid-nintendo
- Specific driver for 3rd party Hori controller: https://gitlab.com/cipitaua/dkms-hid-nintendolic

## Other Userland projects

- [joycond](https://github.com/DanielOgorchock/joycond): A userspace daemon to combine joy-cons from the hid-nintendo kernel driver
- [joycond-cemuhook](https://github.com/joaorb64/joycond-cemuhook): Support for cemuhook's UDP protocol for joycond devices
