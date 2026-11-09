#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
"""Logs a dual-antenna receiver's heading and roll to CSV, as the vibration-free roll reference for
the IMU driving runs (hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md section 8.3, test 4).

    python3 um982_log.py [--port /dev/cu.usbserial-XXXX | COM6] [--baud N] [--out FILE]
    python3 um982_log.py --udp 9999 [--out FILE]

From a serial port (the receiver's USB or a COM port; default: the first port that sends NMEA, at
the first rate that works), or with --udp from the network: an AiO board's PAOGI to the app on
192.168.5.x, port 9999 (stop AgOpenGPS / AgOpenWeb on this laptop first, they hold the port).

One row per heading sentence, as the daemon reads them (core/src/gnss.cpp):
  $xxHPR   heading, pitch, roll: with the antennas side by side the pitch is the vehicle's roll
  $KSXT    heading, pitch (the vehicle's roll), speed, position and heading quality
  $PAOGI   the AiO's (or the daemon's) sentence: dual heading and roll
with the latest speed and course (VTG, RMC, KSXT) and fix (GGA, GNS) beside it, and the Mac's clock
in mac_time, as tm171_log.py writes it, to line the logs up. Every good sentence also goes to a
.nmea file beside the CSV. Press Enter to bump the marker column; Ctrl-C stops.
Nothing is sent to the receiver.
"""
import argparse, os, socket, sys, threading, time

import serial
from serial.tools import list_ports

COLUMNS = ["mac_time", "marker", "src", "utc", "heading", "roll", "quality",
           "speed_kmh", "course", "fix", "sats", "lat", "lon", "alt"]
RATES = (460800, 115200, 230400, 921600, 38400, 9600)
KNOTS_TO_KMH = 1.852


def checked(line):
    """The sentence's fields if its checksum is good (or it has none), else None."""
    line = line.strip()
    if not line.startswith(("$", "#")):
        return None
    body, star, cs = line[1:].partition("*")
    if star:
        x = 0
        for ch in body:
            x ^= ord(ch)
        try:
            if x != int(cs[:2], 16):
                return None
        except ValueError:
            return None
    return body.split(",")


def num(f, i):
    try:
        return float(f[i]) if i < len(f) and f[i] else None
    except ValueError:
        return None


def latlon(f, i):
    """NMEA ddmm.mmmm,N to signed degrees."""
    v = num(f, i)
    if v is None or i + 1 >= len(f):
        return None
    d = int(v / 100)
    deg = d + (v - d * 100) / 60
    return -deg if f[i + 1] in ("S", "W") else deg


class State:
    def __init__(self):
        self.speed = self.course = self.fix = self.sats = None
        self.lat = self.lon = self.alt = None

    def update(self, f):
        """Speed and fix from the sentences that carry them. Returns a row for a heading sentence."""
        name = f[0]
        kind = name[2:] if len(name) == 5 and name[0] in "GB" else name
        if kind == "VTG":
            self.course, k = num(f, 1), num(f, 7)
            if k is not None:
                self.speed = k
        elif kind == "RMC":
            k = num(f, 7)
            if k is not None:
                self.speed = k * KNOTS_TO_KMH
            self.course = num(f, 8)
        elif kind in ("GGA", "GNS"):
            self.lat, self.lon = latlon(f, 2), latlon(f, 4)
            self.fix = f[6] if len(f) > 6 else None
            self.sats, self.alt = (f[7] if len(f) > 7 else None), num(f, 9)
        elif kind == "HPR":
            # $GNHPR,time,heading,pitch,roll,quality,satellites,age,reserved
            return dict(src="HPR", utc=f[1], heading=num(f, 2), roll=num(f, 3),
                        quality=f[5] if len(f) > 5 else "")
        elif name in ("KSXT", "GPKSXT"):
            # $KSXT,YYYYMMDDHHMMSS.ss,lon,lat,alt,heading,pitch,track,speed_kmh,roll,
            #       position quality,heading quality,heading sats,position sats,...
            # Qualities 0 none, 1 single, 2 float, 3 fixed: written as GGA/HPR's 0, 1, 5, 4.
            gga = lambda i: {"2": "5", "3": "4"}.get(f[i], f[i]) if i < len(f) else ""
            self.lon, self.lat, self.alt = num(f, 2), num(f, 3), num(f, 4)
            self.course, self.speed = num(f, 7), num(f, 8)
            self.fix = gga(10) or self.fix
            self.sats = f[13] if len(f) > 13 else self.sats
            return dict(src="KSXT", utc=f[1][8:] if len(f[1]) >= 14 else f[1], heading=num(f, 5),
                        roll=num(f, 6), quality=gga(11))
        elif name == "PAOGI":
            # $PAOGI,time,lat,N,lon,E,fix,sats,hdop,alt,age,speed_knots,heading,roll,pitch,yawrate
            self.lat, self.lon = latlon(f, 2), latlon(f, 4)
            self.fix, self.sats, self.alt = f[6], f[7], num(f, 9)
            k = num(f, 11)
            if k is not None:
                self.speed = k * KNOTS_TO_KMH
            return dict(src="PAOGI", utc=f[1], heading=num(f, 12), roll=num(f, 13), quality=f[6])
        return None


def serial_lines(port):
    """Lines as they come; an empty one on each read timeout, so the status line keeps printing
    while the receiver is silent."""
    buf = b""
    while True:
        data = port.read(4096)
        if not data:
            yield ""
        buf += data
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            yield ln.decode("ascii", "replace")


def udp_lines(sock):
    sock.settimeout(0.5)
    while True:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            yield ""
            continue
        for ln in data.split(b"\n"):
            yield ln.decode("ascii", "replace")


def find_serial(a):
    names = [a.port] if a.port else sorted(p.device for p in list_ports.comports() if p.vid)
    for name in names:
        for rate in ([a.baud] if a.baud else RATES):
            try:
                s = serial.Serial(name, rate, timeout=0.2)
            except serial.SerialException:  # busy: MEMS Studio or the TM171 logger has it
                break
            except (OSError, ValueError):  # a rate this port cannot do
                continue
            t, data = time.time() + 1.5, b""
            while time.time() < t:
                data += s.read(4096)
            good = sum(1 for ln in data.split(b"\n") if checked(ln.decode("ascii", "replace")))
            if good >= 3:
                print("NMEA on %s at %d" % (name, rate))
                return s
            s.close()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="default: the first serial port that sends NMEA")
    ap.add_argument("--baud", type=int, help="default: try %s" % ", ".join(map(str, RATES)))
    ap.add_argument("--udp", type=int, metavar="PORT", help="listen on this UDP port instead (9999: PAOGI)")
    ap.add_argument("--out", default=time.strftime("um982_%Y%m%d_%H%M%S.csv"))
    ap.add_argument("--seconds", type=float, default=0, help="stop after N s (default: Ctrl-C)")
    a = ap.parse_args()

    if a.udp:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("", a.udp))
        except OSError as e:
            sys.exit("UDP %d: %s (is AgOpenGPS or AgOpenWeb running here?)" % (a.udp, e))
        print("listening on UDP", a.udp)
        lines = udp_lines(sock)
    else:
        port = find_serial(a)
        if port is None:
            sys.exit("no NMEA found (try --port and --baud, or --udp 9999)")
        lines = serial_lines(port)

    marker = [0]

    def keys():
        for _ in sys.stdin:
            marker[0] += 1
            print("marker", marker[0], flush=True)
    threading.Thread(target=keys, daemon=True).start()

    st = State()
    rows, seen, last = 0, {}, {}
    start = last_print = time.time()
    raw_path = os.path.splitext(a.out)[0] + ".nmea"
    with open(a.out, "w") as out, open(raw_path, "w") as raw:
        out.write(",".join(COLUMNS) + "\n")
        try:
            for ln in lines:
                now = time.time()
                f = checked(ln)
                if f:
                    raw.write("%.4f %s\n" % (now, ln.strip()))
                    seen[f[0]] = seen.get(f[0], 0) + 1
                    r = st.update(f)
                    if r:
                        r.update(mac_time="%.4f" % now, marker=marker[0], speed_kmh=st.speed, course=st.course,
                                 fix=st.fix, sats=st.sats, lat=st.lat, lon=st.lon, alt=st.alt)
                        out.write(",".join("" if r.get(c) is None else
                                           ("%.8f" % r[c] if c in ("lat", "lon") else
                                            "%.3f" % r[c] if isinstance(r[c], float) else str(r[c]))
                                           for c in COLUMNS) + "\n")
                        rows += 1
                        last = r
                if now - last_print >= 1.0:
                    last_print = now
                    print("%6.0f s  rows %d  heading %s  roll %s  hq %s  speed %s  fix %s  marker %d  [%s]" % (
                        now - start, rows, last.get("heading", "-"), last.get("roll", "-"),
                        last.get("quality") or "-",
                        "%.1f" % st.speed if st.speed is not None else "-", st.fix or "-", marker[0],
                        " ".join("%s %d" % kv for kv in sorted(seen.items()))), flush=True)
                    if now - start > 5 and not rows:
                        print("  no HPR, KSXT or PAOGI yet: the receiver must output one of them", flush=True)
                if a.seconds and now - start >= a.seconds:
                    break
        except KeyboardInterrupt:
            pass
    print("wrote %d rows to %s (all sentences in %s)" % (rows, a.out, raw_path))


if __name__ == "__main__":
    main()
