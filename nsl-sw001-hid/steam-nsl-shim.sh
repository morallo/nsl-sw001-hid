#!/usr/bin/env bash
# Launch Steam with env vars that make the N-SL SW001 work correctly:
#  - SDL_HIDAPI_IGNORE_DEVICES: exclude only the SW001 (057E:2009) from the
#    HIDAPI Nintendo Switch driver (which it can't speak), letting SDL use
#    evdev for it.  Other controllers (DualShock3, Xbox360, Xbox One) keep
#    their HIDAPI drivers.
#  - SDL_GAMECONTROLLERCONFIG: custom gamecontrollerdb entry mapping the
#    SW001's actual axis/button indices to standard SDL gamepad roles.
export SDL_HIDAPI_IGNORE_DEVICES="0x057e/0x2009"
export SDL_GAMECONTROLLERCONFIG="$(cat "$(dirname "$0")/nsl-sw001-controllersdb.txt")"

exec steam "$@"
