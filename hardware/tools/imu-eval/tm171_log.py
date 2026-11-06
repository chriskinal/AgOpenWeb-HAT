#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
"""Logs a TM171 on USB to CSV, as the reference IMU for the LSM6DSK320X evaluation
(hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md section 8).

    python3 tm171_log.py [--port /dev/cu.usbmodemXXXX | COM5] [--out FILE] [--seconds N]

One row per TM171 sample (50 Hz): the Mac's clock (USB delivers frames in bursts, so use dev_ts,
in microseconds, for spacing), roll/pitch/yaw, the raw gyro, accel and magnetometer, temperature.
Press Enter to bump the marker column (still -> shaking -> still); Ctrl-C stops.

TM171 frames: 0xAA 0x55, size, payload, CRC-16/Modbus over size and payload (as core/imu.cpp).
The payload starts with 4 bytes of info (object id in the first byte's low 7 bits) and a 4-byte
timestamp, then little-endian floats:
  0x23  roll, pitch, yaw (deg)
  0x29  gyro x, y, z; accel x, y, z (g); magnetometer x, y, z
  0x16  temperature (deg C), and one more value
"""
import argparse, struct, sys, threading, time

import serial
from serial.tools import list_ports

COLUMNS = ["mac_time", "marker", "dev_ts", "roll", "pitch", "yaw",
           "gx", "gy", "gz", "ax", "ay", "az", "mx", "my", "mz", "temp"]


def crc16(data):
    c = 0xFFFF
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def frames(port):
    """Yields (object id, timestamp, floats) for each frame with a good CRC."""
    buf = bytearray()
    while True:
        buf += port.read(1024)
        while True:
            i = buf.find(b"\xaa\x55")
            if i < 0:
                del buf[:-1]
                break
            del buf[:i]
            if len(buf) < 3 or len(buf) < 5 + buf[2]:
                break
            n = buf[2]
            f = bytes(buf[:5 + n])
            if crc16(f[2:3 + n]) != (f[3 + n] | f[4 + n] << 8) or n < 8:
                del buf[:1]
                continue
            del buf[:5 + n]
            p = f[3:3 + n]
            nf = (n - 8) // 4
            yield p[0] & 0x7F, struct.unpack("<I", p[4:8])[0], struct.unpack("<%df" % nf, p[8:8 + 4 * nf])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="default: the first serial port that sends TM171 frames")
    ap.add_argument("--out", default=time.strftime("tm171_%Y%m%d_%H%M%S.csv"))
    ap.add_argument("--seconds", type=float, default=0, help="stop after N s (default: Ctrl-C)")
    a = ap.parse_args()

    port = None
    # Mac: /dev/cu.usbmodem*; Linux: /dev/ttyACM*; Windows: COMn
    names = [a.port] if a.port else sorted(p.device for p in list_ports.comports() if p.vid)
    for name in names:
        try:
            s = serial.Serial(name, 115200, timeout=0.2)
        except serial.SerialException:  # busy: another program (MEMS Studio) has it
            continue
        t = time.time() + 1.0
        data = b""
        while time.time() < t:
            data += s.read(512)
        if data.count(b"\xaa\x55") >= 5:
            port = s
            print("TM171 on", name)
            break
        s.close()
    if port is None:
        sys.exit("no TM171 found (try --port)")

    marker = [0]

    def keys():
        for _ in sys.stdin:
            marker[0] += 1
            print("marker", marker[0], flush=True)
    threading.Thread(target=keys, daemon=True).start()

    row, last, cur_ts, rows = {}, {}, None, 0
    start = time.time()
    last_print = start
    with open(a.out, "w") as out:
        out.write(",".join(COLUMNS) + "\n")
        try:
            for oid, ts, v in frames(port):
                now = time.time()
                if ts != cur_ts and row:
                    out.write(",".join(str(row.get(c, "")) for c in COLUMNS) + "\n")
                    rows += 1
                    last, row = row, {}
                cur_ts = ts
                row.update(mac_time="%.4f" % now, marker=marker[0], dev_ts=ts)
                if oid == 0x23 and len(v) >= 3:
                    row.update(zip(("roll", "pitch", "yaw"), ("%.4f" % x for x in v[:3])))
                elif oid == 0x29 and len(v) >= 9:
                    row.update(zip(COLUMNS[6:15], ("%.6f" % x for x in v[:9])))
                elif oid == 0x16 and v:
                    row["temp"] = "%.2f" % v[0]
                if now - last_print >= 1.0:
                    last_print = now
                    print("%6.0f s  rows %d  roll %s  pitch %s  yaw %s  marker %d" % (
                        now - start, rows, last.get("roll", "-"), last.get("pitch", "-"),
                        last.get("yaw", "-"), marker[0]), flush=True)
                if a.seconds and now - start >= a.seconds:
                    break
        except KeyboardInterrupt:
            pass
    print("wrote %d rows to %s" % (rows, a.out))


if __name__ == "__main__":
    main()
