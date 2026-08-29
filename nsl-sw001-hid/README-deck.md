# N-SL SW001 driver on Steam Deck (SteamOS)

Makes the N-SL SW001 Bluetooth "Pro Controller" clone work on a Steam Deck
running SteamOS. This is **step 1**: get the existing `hid-nsl-sw001` driver
installed and working on the Deck. No IMU/gyro work is included yet.

## What it does

- Builds and installs `hid-nsl-sw001` via **DKMS** against the exact running
  kernel.
- **Globally blacklists `hid_nintendo`** so it stops claiming the SW001 (it
  matches 057e:2009 but can't speak Nintendo's protocol, times out with -110
  and never releases the device).
- Installs the SDL/Steam Input environment so Steam sees the controller
  correctly (via `~/.config/environment.d/nsl-sw001.conf`).
- Re-enables the read-only rootfs after install.
- Survives SteamOS A/B updates via a self-seeding ensure service.

## Files

| File | Purpose |
|---|---|
| `dkms.conf` | DKMS packaging metadata |
| `install-nsl-sw001-deck.sh` | Installer (pass `--uninstall` to remove) |
| `steamos-nsl-ensure.sh` | Boot-time self-seeding rebuild script |
| `steamos-nsl-ensure.service` | systemd unit that runs it at boot |
| `blacklist-hid-nintendo.conf` | Global modprobe blacklist of `hid_nintendo` |
| `modules-load-nsl-sw001.conf` | Load `hid-nsl-sw001` at boot |
| `atomic-update-additional-keep-list.conf` | Makes `hid_nintendo` blacklist + unit survive SteamOS updates |
| `environment-nsl-sw001.conf` | `SDL_HIDAPI_IGNORE_DEVICES` + `SDL_GAMECONTROLLERCONFIG` |
| `README-deck.md` | This file |

## Install

```bash
sudo steamos-readonly disable    # optional; the installer does this itself
./install-nsl-sw001-deck.sh      # then re-enables read-only
```

Options:

- `--keep-writable` — leave the rootfs writable after install.
- `--uninstall` — remove the driver, config, service and SDL env.

After install, fully restart Steam (or log out/in) so the SDL environment.d
vars take effect.

## Verify

```bash
# Controller bound by our driver (not hid-nintendo/hid-generic):
ls /sys/bus/hid/drivers/hid-nsl-sw001/
# Module loaded:
lsmod | grep hid_nsl
# BD_ADDR of the bound device:
cat /sys/bus/hid/devices/0005:057E:2009.*/uniq   # expect your SW001 MAC
# Buttons/axes fire:
evtest
```

## How persistence across A/B updates works

SteamOS swaps the whole root partition (`/usr`) on each update, so anything
there (DKMS sources, built `.ko`, config files) is wiped. `steamos-readonly`
only adds a write-protection toggle; it does not make changes survive.

The only things that survive an update are `/home` (never touched) and
`/etc` (an overlay in `/var`, but only files on Valve's atomic-update
keep-list are carried over).

So the installer:

1. Seeds a **persistent copy** under `/home/.steamos-nsl-sw001/` (source +
   config + ensure script + unit).
2. Installs `steamos-nsl-ensure.service` in `/etc/systemd/system/` and adds it
   to the atomic-update keep-list so it runs on future boots.
3. At each boot, `steamos-nsl-ensure.sh` re-seeds `/usr` and `/etc` from the
   `/home` copy, re-pins the matching kernel headers, runs `dkms build/install`
   for the current kernel, and loads `hid-nsl-sw001`.

This is the same self-healing pattern Valve's NVIDIA installer uses.

## Trade-offs and notes

- **The global `hid_nintendo` blacklist also disables genuine Pro
  Controllers.** This is fine for the current setup (no genuine controllers in
  use). If you ever use a genuine controller, replace the global blacklist with
  the per-device udev steer rule (planned for a later step) instead.
- **SDL `HIDAPI_IGNORE_DEVICES` is strictly VID/PID based** (057e/2009), so a
  genuine Pro Controller on the same Deck would also fall back to evdev and
  lose HIDAPI gyro/rumble. Revisit when/if a genuine controller is used.
- The `hid-nsl-sw001` driver binds the SW001 over `hid-generic` because
  hid-generic declines devices that have a matching special driver.
