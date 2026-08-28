#!/usr/bin/env python3
#
# capdecode.py - decode N-SL SW001 HID input reports from a Bluetooth capture
#
# Reads either:
#   * Android btsnoop ("btsnoop" header, datalink type 1002), or
#   * btmon --write=btsnoop style capture (datalink 2001, BlueZ monitor)
#
# The SW001 streams its input on the HID-interrupt L2CAP channel (CID 0x0041
# in the Linux btmon capture, 0x0049 in the Android one).  Each report is
# wrapped on the wire as:  <a1><report id><report bytes>.
#
# Usage:
#   python3 capdecode.py capture.btsnoop            # button/d-pad press test
#   python3 capdecode.py --sweep capture.btsnoop    # stick sweep test
#   python3 capdecode.py --all   capture.btsnoop    # include seq/gyro noise
#
# Output:
#   * which L2CAP channel and report ids were seen
#   * (press test) the byte/bit transitions of the 0x30 report control bytes
#     vs time, so you can correlate them with what you pressed
#   * (--sweep) the value range of every data byte, so the analog stick bytes
#     (wide range) stand out from the button bytes (narrow range)
#
# Confirmed button map (from the user's controlled capture: presses were
# A B X Y L R ZL ZR SL SR Minus Plus Home, then D-pad Up Right Down Left;
# Capture confirmed separately on payload[5] bit5):
#   payload[4] (data byte 2): bit0=Y bit1=X bit2=B bit3=A
#                             bit6=R bit7=ZR   (bit4/5 spare)
#   payload[5] (data byte 3): bit0=Minus bit1=Plus bit2=SR bit3=SL
#                             bit4=Home bit5=Capture  (bit6/7 spare)
#   payload[6] (data byte 4): bit0=Dwn bit1=Up bit2=Right bit3=Left
#                             bit6=L bit7=ZL   (bit4/5 spare)
#   sticks: best candidates are payload[7..12] (6 bytes = 4x 12-bit packed,
#           JoyCon-style). To confirm: do a capture where you sweep ONLY the
#           left stick full deflection, controller held still, then ONLY the
#           right stick. The bytes that move are LX/LY and RX/RY. Use
#           capdecode.py --sweep + compare min/max vs a still capture.
#
# Recommended capture procedure (records one press at a time):
#   1.  sudo btmon -w ctrl.btsnoop &
#   2.  connect the controller over Bluetooth (wind back / re-pair if needed)
#   3.  press buttons one at a time, ~1 s apart: A B X Y L R ZL ZR SL SR
#       Minus Plus Home Capture
#   4.  push each D-pad direction: Up Right Down Left
#   5.  push each stick full deflection left/right/up/down, back to center
#       between each
#   6.  kill btmon and run:  python3 capdecode.py ctrl.btsnoop
#
# Then fill in the button/axis map from the printed bit transitions.  Do NOT
# wiggle the controller while capturing: gyroscope bytes are mixed into the
# early part of the report and will flood the output with noise.

import struct
import sys
from collections import Counter

WRAP = 0xA1          # HID-profile data wrapper byte on the interrupt channel
RID_GAME = 0x30      # main streaming input report id


def load(fn):
    f = open(fn, "rb")
    hdr = f.read(16)
    if hdr[:8] != b"btsnoop\x00":
        raise SystemExit(f"{fn}: not a btsnoop file")
    ver, dtype = struct.unpack(">II", hdr[8:16])
    recs = []
    while True:
        rh = f.read(24)
        if len(rh) != 24:
            break
        ol, il, fl, drops, ts = struct.unpack(">IIIIq", rh)
        recs.append((ts, fl, f.read(il)))
    return dtype, recs


def parse_capture(fn):
    dtype, recs = load(fn)
    out = []
    if dtype == 1002:                      # Android: <type byte><HCI packet>
        for ts, fl, d in recs:
            if len(d) >= 5 and d[0] == 0x02:   # HCI ACL data
                body = d[1:]
                h, l = struct.unpack("<HH", body[:4])
                if len(body) == 4 + l:
                    py = body[4:]
                    if len(py) >= 4:
                        ll, cid = struct.unpack("<HH", py[:4])
                        if ll + 4 <= len(py):
                            out.append((ts, fl, h & 0xFFF, cid, py[4:4 + ll]))
    elif dtype == 2001:                    # btmon native: raw HCI ACL packets
        for ts, fl, d in recs:
            cands = []
            if len(d) >= 4 and len(d) == 4 + struct.unpack("<H", d[2:4])[0]:
                h, l = struct.unpack("<HH", d[:4])
                cands.append((d[4:], h))
            if len(d) >= 6:                # a few records carry monitor hdr
                op, idx, l = struct.unpack(">HHH", d[:6])
                if op in (3, 4) and len(d) - 6 == l:
                    hh, ll = struct.unpack("<HH", d[6:10])
                    cands.append((d[10:6 + l], hh))
            for py, h in cands:
                if len(py) >= 4:
                    ll, cid = struct.unpack("<HH", py[:4])
                    if ll + 4 <= len(py):
                        out.append((ts, fl, h & 0xFFF, cid, py[4:4 + ll]))
    else:
        raise SystemExit(f"unsupported datalink type {dtype} in {fn}")
    return out


def main():
    args = sys.argv[1:]
    fn = next((a for a in args if not a.startswith("--")), None)
    if fn is None:
        raise SystemExit("usage: capdecode.py [--sweep|--all] capture.btsnoop")
    if "--sweep" in args:
        sweep(fn)
        return
    pk = parse_capture(fn)
    cids = Counter(r[3] for r in pk)
    print("L2CAP channels:", {hex(k): v for k, v in cids.items()})

    seen = set()
    for r in pk:
        if len(r[4]) >= 2 and r[4][0] == WRAP:
            seen.add((r[3], r[4][1]))
    print("reports on wrapped channels:", sorted(seen))

    cand = [c for c in cids if any(r[3] == c and r[4][:2] == bytes([WRAP, RID_GAME])
                                   for r in pk)]
    if not cand:
        raise SystemExit("no report 0x30 found")
    cid = cand[0]
    rs = sorted((r for r in pk if r[3] == cid and r[4][:2] ==
                 bytes([WRAP, RID_GAME])), key=lambda r: r[0])
    t0 = rs[0][0] / 1e6
    print(f"using channel 0x{cid:02x}, {len(rs)} x report 0x30 "
          f"over {len(rs)*1.0:.0f} reports")
    print("  wire: a1 30 | seq | f1 | c0 | c1 | c2 | m6 | rest.. | 0a | imu*3")
    print("  (c0..c2 are the interesting control bytes; m6 = motion/gyro pair)")

    last = None
    ctrl = range(3, 7)                     # interesting control bytes
    allb = range(2, 14) if "--all" in sys.argv else ctrl
    counts = Counter()
    for t, _fl, _h, _cid, p in rs:
        b = p[2:]
        if last is None:
            last = b
            continue
        dt = t / 1e6 - t0
        for i in allb:                     # control area only
            d = b[i - 2] ^ last[i - 2]
            if not d:
                continue
            counts[i] += 1
            bits = [k for k in range(8) if d & (1 << k)]
            for k in bits:
                print("  t=%8.3f  byte=%2d  bit%d  %s -> %s%s" % (
                    dt, i, k,
                    "1" if (last[i - 2] >> k) & 1 else "0",
                    "1" if (b[i - 2] >> k) & 1 else "0",
                    "   <- controls/buttons" if 3 <= i <= 6 else ""))
        last = b
    print("\n  transitions per byte:", " ".join("b%d=%d" % (k, c)
                                               for k, c in sorted(counts.items())))


def sweep(fn):
    pk = parse_capture(fn)
    cand = [c for c in set(r[3] for r in pk)
            if any(r[3] == c and r[4][:2] == bytes([WRAP, RID_GAME])
                   for r in pk)]
    if not cand:
        raise SystemExit("no report 0x30 found")
    cid = cand[0]
    rs = sorted((r for r in pk if r[3] == cid and r[4][:2] ==
                 bytes([WRAP, RID_GAME])), key=lambda r: r[0])
    lo = [255] * 48
    hi = [0] * 48
    vals = [set() for _ in range(48)]
    for _, _fl, _h, _c, p in rs:
        b = p[2:]
        for i, v in enumerate(b):
            vals[i].add(v)
            if v < lo[i]:
                lo[i] = v
            if v > hi[i]:
                hi[i] = v
    print("  data byte | min  max  | distinct values")
    for i in range(48):
        tag = "   <- analog?" if hi[i] - lo[i] > 15 else ""
        print("   %2d       %3d  %3d  %s%s" % (
            i, lo[i], hi[i], ",".join(hex(x) for x in sorted(vals[i]))[:36],
            tag))
    print("\n  bytes with a wide range are the analog stick axes;")
    print("  bytes with a few values are buttons (bit = unused/unknown).")


if __name__ == "__main__":
    main()