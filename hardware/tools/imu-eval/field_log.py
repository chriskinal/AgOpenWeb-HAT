#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
"""Logs the TM171 and, for driving runs, a dual-antenna receiver's heading and roll.

The reference devices for the LSM6DSK320X evaluation (hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md
section 8), each to its own CSV, with one marker and one status line.

    python3 field_log.py --out ~/imu-runs/r01                 TM171 alone: r01_tm171.csv
    python3 field_log.py --out ~/imu-runs/r10 --um982         and the receiver: r10_um982.csv
    python3 field_log.py --out ~/imu-runs/r10 --udp 9999      the receiver's PAOGI from an AiO

Ports are found by listening (the TM171's frames at 115200; NMEA at the first rate that works), or
given: --tm171 /dev/cu.usbmodemXXXX | COM5, --um982 /dev/cu.usbserial-XXXX | COM6 [--baud N].
--udp takes the receiver from the network: an AiO board's PAOGI to the app on 192.168.5.x, port
9999 (stop AgOpenGPS / AgOpenWeb on this laptop first, they hold the port).
Every row carries the laptop's clock (mac_time), so both logs and MEMS Studio's line up. Press Enter
to bump the marker column in both (still -> shaking -> still); Ctrl-C stops.

TM171 log, one row per sample (50 Hz): roll/pitch/yaw, the raw gyro, accel and magnetometer,
temperature, and dev_ts, the TM171's own timestamp in microseconds (USB delivers frames in bursts,
so use it for spacing). Frames: 0xAA 0x55, size, payload, CRC-16/Modbus over size and payload (as
core/imu.cpp). The payload starts with 4 bytes of info (object id in the first byte's low 7 bits)
and a 4-byte timestamp, then little-endian floats:
  0x23  roll, pitch, yaw (deg)
  0x29  gyro x, y, z; accel x, y, z (g); magnetometer x, y, z
  0x16  temperature (deg C), and one more value

Receiver log, one row per heading sentence, as the daemon reads them (core/src/gnss.cpp):
  $xxHPR   heading, pitch, roll: with the antennas side by side the pitch is the vehicle's roll
  $KSXT    heading, pitch (the vehicle's roll), speed, position and heading quality
  $PAOGI   the AiO's (or the daemon's) sentence: dual heading and roll
with the latest speed and course (VTG, RMC, KSXT) and fix (GGA, GNS, KSXT) beside it. Qualities
are in GGA's numbers (4 fixed, 5 float). Every good sentence also goes to a .nmea file. Nothing is
sent to either device.
"""
import argparse, os, socket, struct, sys, threading, time

import serial
from serial.tools import list_ports

TM_COLUMNS = ["mac_time", "marker", "dev_ts", "roll", "pitch", "yaw",
              "gx", "gy", "gz", "ax", "ay", "az", "mx", "my", "mz", "temp"]
RX_COLUMNS = ["mac_time", "marker", "src", "utc", "heading", "roll", "quality",
              "speed_kmh", "course", "fix", "sats", "lat", "lon", "alt"]
RATES = (115200, 460800, 230400, 921600, 38400, 9600)
KNOTS_TO_KMH = 1.852


# --- TM171 ---

def crc16(data):
    c = 0xFFFF
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def frames(port):
    """Yields (object id, timestamp, floats) for each frame with a good CRC; None on each read
    timeout, so the caller can stop."""
    buf = bytearray()
    while True:
        data = port.read(1024)
        if not data:
            yield None
        buf += data
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


class Device:
    """A serial device read on its own thread; reopened by name if it drops (a lead knocked out)."""

    def __init__(self, port, path):
        self.port, self.path = port, path
        self.rows, self.last, self.drops = 0, {}, 0

    def run(self, marker, stop):
        with self.open_files() as out:
            while not stop.is_set():
                try:
                    self.read(out, marker, stop)
                except (serial.SerialException, OSError):
                    self.drops += 1
                    self.port = reopen(self.port, stop)
                    if self.port is None:
                        break

    def dropped(self):
        return "  dropped %d" % self.drops if self.drops else ""


def reopen(port, stop):
    name, rate = port.port, port.baudrate
    try:
        port.close()
    except Exception:
        pass
    while not stop.wait(1.0):
        try:
            return serial.Serial(name, rate, timeout=0.2)
        except (serial.SerialException, OSError):
            pass
    return None


class Tm171(Device):
    name = "TM171"

    def open_files(self):
        out = open(self.path, "w")
        out.write(",".join(TM_COLUMNS) + "\n")
        return out

    def read(self, out, marker, stop):
        row, cur_ts = {}, None
        for fr in frames(self.port):
            if stop.is_set():
                break
            if fr is None:
                continue
            oid, ts, v = fr
            now = time.time()
            if ts != cur_ts and row:
                out.write(",".join(str(row.get(c, "")) for c in TM_COLUMNS) + "\n")
                self.rows += 1
                self.last, row = row, {}
            cur_ts = ts
            row.update(mac_time="%.4f" % now, marker=marker[0], dev_ts=ts)
            if oid == 0x23 and len(v) >= 3:
                row.update(zip(("roll", "pitch", "yaw"), ("%.4f" % x for x in v[:3])))
            elif oid == 0x29 and len(v) >= 9:
                row.update(zip(TM_COLUMNS[6:15], ("%.6f" % x for x in v[:9])))
            elif oid == 0x16 and v:
                row["temp"] = "%.2f" % v[0]

    def status(self):
        return "TM171 rows %d  roll %s  pitch %s%s" % (
            self.rows, self.last.get("roll", "-"), self.last.get("pitch", "-"), self.dropped())


# --- Receiver ---

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


class Nav:
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
    """Lines as they come; an empty one on each read timeout."""
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
    sock.settimeout(0.2)
    while True:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            yield ""
            continue
        for ln in data.split(b"\n"):
            yield ln.decode("ascii", "replace")


class Receiver(Device):
    """From a serial port, or with sock (UDP) from the network."""
    name = "receiver"

    def __init__(self, port, path, sock=None):
        Device.__init__(self, port, path)
        self.sock = sock
        self.raw_path = os.path.splitext(path)[0] + ".nmea"
        self.seen, self.nav = {}, Nav()

    def open_files(self):
        out, self.raw = open(self.path, "w"), open(self.raw_path, "w")
        out.write(",".join(RX_COLUMNS) + "\n")
        return out

    def read(self, out, marker, stop):
        nav, raw = self.nav, self.raw
        for ln in (udp_lines(self.sock) if self.sock else serial_lines(self.port)):
            if stop.is_set():
                break
            f = checked(ln)
            if not f:
                continue
            now = time.time()
            raw.write("%.4f %s\n" % (now, ln.strip()))
            self.seen[f[0]] = self.seen.get(f[0], 0) + 1
            r = nav.update(f)
            if r:
                r.update(mac_time="%.4f" % now, marker=marker[0], speed_kmh=nav.speed, course=nav.course,
                         fix=nav.fix, sats=nav.sats, lat=nav.lat, lon=nav.lon, alt=nav.alt)
                out.write(",".join("" if r.get(c) is None else
                                   ("%.8f" % r[c] if c in ("lat", "lon") else
                                    "%.3f" % r[c] if isinstance(r[c], float) else str(r[c]))
                                   for c in RX_COLUMNS) + "\n")
                self.rows += 1
                self.last = r

    def run(self, marker, stop):
        try:
            Device.run(self, marker, stop)
        finally:
            self.raw.close()

    def status(self):
        nav = self.nav
        return "receiver rows %d  heading %s  roll %s  hq %s  fix %s  speed %s%s  [%s]" % (
            self.rows, self.last.get("heading", "-"), self.last.get("roll", "-"),
            self.last.get("quality") or "-", nav.fix or "-",
            "%.1f" % nav.speed if nav.speed is not None else "-", self.dropped(),
            " ".join("%s %d" % kv for kv in sorted(self.seen.items())))


# --- Finding the ports ---

def listen(name, rate, secs):
    """What the port sends in secs at rate: (open port, bytes), or (None, b"") if it will not open."""
    try:
        s = serial.Serial(name, rate, timeout=0.2)
    except serial.SerialException:  # busy: another program (MEMS Studio) has it
        return None, b""
    except (OSError, ValueError):  # a rate this port cannot do
        return None, b""
    t, data = time.time() + secs, b""
    while time.time() < t:
        data += s.read(4096)
    return s, data


def is_tm171(data):
    return data.count(b"\xaa\x55") >= 5


def is_nmea(data):
    return sum(1 for ln in data.split(b"\n") if checked(ln.decode("ascii", "replace"))) >= 3


def find_ports(a):
    """Opens the TM171 and, if asked for, the receiver. Each port is listened to once at 115200;
    only a port that sent neither is tried at the receiver's other rates, so the scan reads as
    little as it can from ports another program has (on a Mac, opening a busy port does not fail)."""
    want_rx = a.um982 is not None
    tm = rx = None
    if a.tm171:
        s, data = listen(a.tm171, 115200, 1.0)
        if s and is_tm171(data):
            tm = s
        elif s:
            s.close()
    if want_rx and a.um982 != "auto":
        for rate in ([a.baud] if a.baud else RATES):
            s, data = listen(a.um982, rate, 1.5)
            if s and is_nmea(data):
                rx = s
                break
            if s:
                s.close()
    taken = {a.tm171, a.um982}
    names = [p for p in sorted(p.device for p in list_ports.comports() if p.vid) if p not in taken]
    quiet = []
    for name in names:
        if tm and (rx or not want_rx):
            break
        s, data = listen(name, 115200, 1.0)
        if not s:
            continue
        if not tm and is_tm171(data):
            tm = s
            continue
        if want_rx and not rx and a.um982 == "auto" and is_nmea(data):
            rx = s
            continue
        s.close()
        quiet.append(name)
    if want_rx and not rx and a.um982 == "auto":
        for name in quiet:
            for rate in ([a.baud] if a.baud else RATES[1:]):
                s, data = listen(name, rate, 1.5)
                if s and is_nmea(data):
                    rx = s
                    break
                if s:
                    s.close()
            if rx:
                break
    return tm, rx


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=time.strftime("run_%Y%m%d_%H%M%S"),
                    help="file name start: PREFIX_tm171.csv, PREFIX_um982.csv")
    ap.add_argument("--tm171", metavar="PORT", help="default: the first serial port that sends TM171 frames")
    ap.add_argument("--um982", metavar="PORT", nargs="?", const="auto",
                    help="log the receiver too; default port: the first that sends NMEA")
    ap.add_argument("--baud", type=int, help="the receiver's rate (default: try %s)" % ", ".join(map(str, RATES)))
    ap.add_argument("--udp", type=int, metavar="PORT", help="the receiver from this UDP port instead (9999: PAOGI)")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N s (default: Ctrl-C)")
    a = ap.parse_args()
    prefix = a.out[:-4] if a.out.endswith(".csv") else a.out
    if a.udp:
        a.um982 = None

    tm, rx_port = find_ports(a)
    if tm is None:
        sys.exit("no TM171 found (try --tm171 PORT)")
    print("TM171 on", tm.port)
    devices = [Tm171(tm, prefix + "_tm171.csv")]
    if a.udp:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("", a.udp))
        except OSError as e:
            sys.exit("UDP %d: %s (is AgOpenGPS or AgOpenWeb running here?)" % (a.udp, e))
        print("receiver on UDP", a.udp)
        devices.append(Receiver(None, prefix + "_um982.csv", sock))
    elif a.um982:
        if rx_port is None:
            sys.exit("no receiver found (try --um982 PORT and --baud, or --udp 9999)")
        print("receiver on %s at %d" % (rx_port.port, rx_port.baudrate))
        devices.append(Receiver(rx_port, prefix + "_um982.csv"))

    marker, stop = [0], threading.Event()

    def keys():
        for _ in sys.stdin:
            marker[0] += 1
            print("marker", marker[0], flush=True)
    threading.Thread(target=keys, daemon=True).start()
    threads = [threading.Thread(target=d.run, args=(marker, stop)) for d in devices]
    for t in threads:
        t.start()

    start = time.time()
    seen = {d.name: (0, start) for d in devices}  # rows, and when they last went up
    try:
        while not (a.seconds and time.time() - start >= a.seconds):
            time.sleep(1.0)
            now = time.time()
            print("%6.0f s  marker %d  %s" % (now - start, marker[0], "  |  ".join(d.status() for d in devices)),
                  flush=True)
            for d, t in zip(devices, threads):
                if d.rows != seen[d.name][0]:
                    seen[d.name] = (d.rows, now)
                elif now - seen[d.name][1] > 3:
                    print("  no %s rows for %.0f s%s" % (d.name, now - seen[d.name][1],
                          ": the receiver must output HPR, KSXT or PAOGI" if d.name == "receiver" and not d.rows
                          else ": is its lead in?"), flush=True)
    except KeyboardInterrupt:
        pass
    stop.set()
    for t in threads:
        t.join(2.0)
    for d in devices:
        print("wrote %d rows to %s" % (d.rows, d.path) +
              (" (all sentences in %s)" % d.raw_path if isinstance(d, Receiver) else ""))


if __name__ == "__main__":
    main()
