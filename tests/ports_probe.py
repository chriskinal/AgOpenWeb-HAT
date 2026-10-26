#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""End-to-end test of the serial port settings with no hardware: pseudo-terminals stand in for
the GNSS receiver and the two RS-232 ports.

Outputs: a settings file puts NMEA out (GGA only, 5 Hz) on RS-232 #1 and RTCM out on #2; it
streams GGA and VTG at 10 Hz into the receiver and checks #1 gets every other GGA, no VTG, and
sends RTCM to the corrections port and checks #2 gets it byte for byte, as the receiver does.

Corrected position: NMEA out (GGA, VTG, RMC) on #1; GGA and VTG at 10 Hz, each followed by a
PGN 100 from the app on UDP. Every GGA and RMC out carries PGN 100's position to 7 decimals of a
minute, VTG and RMC its heading, checksums valid; PGN 100 stopped, the receiver's own GGA is
back within about a second.

Detection: no outputs; then
  - RTCM into #1: found to be a radio, and its RTCM reaches the receiver;
  - text into #2: not recognised, its lines shown;
  - BNO085 packets into #2: found to be an IMU;
  - the onboard receiver silent and GGA into #1 (the radio unplugged and a receiver plugged in):
    found to be a receiver after the radio is lost, and it is the one in use.
Own ports (8889, 2234, web 8090), so it can run beside the service.

    python3 tests/ports_probe.py build/aow-hatd
"""
import functools
import json
import os
import pty
import select
import socket
import struct
import subprocess
import sys
import time
import tty
import urllib.request


def nmea(body):
    cs = functools.reduce(lambda a, c: a ^ ord(c), body, 0)
    return ("$%s*%02X\r\n" % (body, cs)).encode()


def pgn100(lat, lon, heading):
    data = struct.pack("<ddd", lon, lat, heading)
    frame = bytes([0x80, 0x81, 0x7F, 100, len(data)]) + data
    return frame + bytes([sum(frame[2:]) & 0xFF])


def degmin(v, digits):
    units = round(abs(v) * 60 * 10**7)
    d, rest = divmod(units, 60 * 10**7)
    return "%0*d%02d.%07d" % (digits, d, rest // 10**7, rest % 10**7)


def checksum_ok(line):
    body, _, cs = line[1:].partition("*")
    return cs == "%02X" % functools.reduce(lambda a, c: a ^ ord(c), body, 0)


def new_pty():
    m, s = pty.openpty()
    tty.setraw(m)
    tty.setraw(s)
    return m, s


def read_all(fd, wait=0.2):
    out = b""
    end = time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0.0, end - time.time()))
        if r:
            out += os.read(fd, 4096)
    return out


def rtcm_frame(msg):
    body = bytes([msg >> 4, (msg & 0x0F) << 4]) + bytes(6)
    f = bytes([0xD3, 0, len(body)]) + body
    crc = 0
    for b in f:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return f + bytes([(crc >> 16) & 0xFF, (crc >> 8) & 0xFF, crc & 0xFF])


def bno(index):
    body = bytes([index & 0xFF]) + bytes(15)
    return b"\xAA\xAA" + body + bytes([sum(body) & 0xFF])


def status():
    return json.load(urllib.request.urlopen("http://127.0.0.1:8090/api/status", timeout=2))


def port(st, name):
    return next((p for p in st["serial"]["ports"] if p["name"] == name), {})


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    gps_m, gps_s = new_pty()
    p1_m, p1_s = new_pty()
    p2_m, p2_s = new_pty()
    cfg = "/tmp/ports_probe_settings.json"
    with open(cfg, "w") as f:
        json.dump({"ports": {"nmeaOut": "port1", "rtcmOut": "port2", "rate": [38400, 0], "nmeaSentences": 1,
                             "nmeaRateHz": 5}}, f)

    args = [daemon, "--gnss", os.ttyname(gps_s), "--gnss-baud", "115200",
            "--rs232", "%s,%s" % (os.ttyname(p1_s), os.ttyname(p2_s)),
            "--listen-port", "8889", "--rtcm-port", "2234", "--web-port", "8090",
            "--gvret-port", "0", "--config", cfg]
    proc = subprocess.Popen(args)
    time.sleep(0.5)
    ok = True

    # 2 s of GGA and VTG at 10 Hz: NMEA out gets GGA only, about 5 a second.
    got = b""
    for i in range(20):
        t = time.gmtime()
        stamp = time.strftime("%H%M%S", t) + ".%d0" % (i % 10)
        os.write(gps_m, nmea("GNVTG,054.7,T,034.4,M,005.5,N,010.2,K,A"))
        os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % stamp))
        got += read_all(p1_m, 0.1)
    got += read_all(p1_m, 0.2)
    lines = [l for l in got.decode(errors="replace").split("\r\n") if l]
    gga = [l for l in lines if l.startswith("$GNGGA")]
    print("NMEA out: %d sentences, %d GGA, first %s" % (len(lines), len(gga), lines[0] if lines else "-"))
    ok &= len(gga) == len(lines) and 8 <= len(gga) <= 12

    # RTCM from the app's port: to the receiver and to RTCM out, the same bytes.
    rtcm = bytes([0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x02, 0x02, 0x55, 0xAA])
    read_all(gps_m, 0.05)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(rtcm, ("127.0.0.1", 2234))
    out = read_all(p2_m, 0.3)
    to_rx = read_all(gps_m, 0.1)
    print("RTCM out: %d bytes, receiver %d bytes" % (len(out), len(to_rx)))
    ok &= out == rtcm and to_rx == rtcm

    st = status()
    print("status serial: %s" % {k: v for k, v in st["serial"].items() if k != "ports"})
    ok &= st["serial"]["gnssPort"] == os.ttyname(gps_s) and st["serial"]["nmeaOutSent"] >= 8
    ok &= st["serial"]["rtcmOutBytes"] == len(rtcm)
    # Going Out Now names RTCM message numbers: real frames through (the first may be taken as
    # the rest of the cut-off test frame above, as after a dropped byte in a real stream).
    for _ in range(3):
        s.sendto(rtcm_frame(1005), ("127.0.0.1", 2234))
        time.sleep(0.1)
    time.sleep(0.2)
    types = status()["serial"]["rtcmOutTypes"]
    print("RTCM out types: %s" % types)
    ok &= types == [1005]
    proc.terminate()
    proc.wait(timeout=5)

    # Corrected position (PGN 100).
    with open(cfg, "w") as f:
        json.dump({"ports": {"nmeaOut": "port1", "rate": [38400, 0], "nmeaSentences": 7}}, f)
    proc = subprocess.Popen(args)
    time.sleep(0.5)
    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    got = b""
    sent = {}
    for i in range(20):
        stamp = time.strftime("%H%M%S", time.gmtime()) + ".%d0" % (i % 10)
        os.write(gps_m, nmea("GNVTG,054.7,T,034.4,M,005.5,N,010.2,K,A"))
        os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % stamp))
        time.sleep(0.02)
        lat, lon, hdg = 48.123456789 + i * 1e-7, -96.987654321 - i * 1e-7, 10.0 + i
        sent[stamp] = (lat, lon, hdg)
        app.sendto(pgn100(lat, lon, hdg), ("127.0.0.1", 8889))
        got += read_all(p1_m, 0.08)
    st = status()["serial"]
    got += read_all(p1_m, 0.1)
    lines = [l for l in got.decode(errors="replace").split("\r\n") if l]
    good = 0
    vtg_heading = None
    for l in lines:
        f = l.split("*")[0].split(",")
        if not checksum_ok(l):
            continue
        if f[0] == "$GNGGA" and f[1] in sent:
            lat, lon, hdg = sent[f[1]]
            good += f[2:6] == [degmin(lat, 2), "N", degmin(lon, 3), "W"]
            last = hdg
        elif f[0] == "$GNVTG":
            vtg_heading = f[1]
    print("corrected: %d lines, %d GGA with PGN 100's position, last VTG course %s, status %s" %
          (len(lines), good, vtg_heading, {k: st[k] for k in ("nmeaSource", "correctedFixes", "correctedSent")}))
    ok &= len(lines) >= 36 and good >= 18 and all(checksum_ok(l) for l in lines)
    ok &= vtg_heading == "%05.1f" % last and st["nmeaSource"] == "corrected" and st["correctedFixes"] == 20
    # Going Out Now: the last GGA and VTG written, as written.
    shown = st.get("nmeaOutLast", [])
    print("going out now: %s" % shown)
    ok &= len(shown) == 2 and shown[0].startswith("$GNGGA,") and ",W,4," in shown[0] and shown[1].startswith("$GNVTG,%05.1f" % last)

    # PGN 100 stops: the receiver's own position again within about a second.
    stopped = time.time()
    back = None
    while time.time() - stopped < 2.5 and back is None:
        os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % time.strftime("%H%M%S.00")))
        if b"4807.0380,N,01131.0000,E" in read_all(p1_m, 0.1):
            back = time.time() - stopped
    print("receiver's position back after %s s, source %s" % (back and round(back, 2), status()["serial"]["nmeaSource"]))
    ok &= back is not None and 0.8 <= back <= 1.4 and status()["serial"]["nmeaSource"] == "receiver"
    proc.terminate()
    proc.wait(timeout=5)

    # Detection.
    with open(cfg, "w") as f:
        json.dump({"ports": {}}, f)
    proc = subprocess.Popen(args)
    time.sleep(0.5)
    frame = rtcm_frame(1077)
    for _ in range(3):
        os.write(p1_m, frame)
        time.sleep(0.1)
    port1 = port(status(), "Port 1")
    to_rx = read_all(gps_m, 0.2)
    print("RTCM into #1: %s %s; receiver got %d bytes" % (port1["kind"], port1["rtcm"], len(to_rx)))
    ok &= port1["kind"] == "radio" and port1["rtcm"] == [1077] and frame in to_rx

    os.write(p2_m, b"SOME DEVICE TALKING HERE 1\r\nLINE TWO\r\n")
    time.sleep(0.3)
    port2 = port(status(), "Port 2")
    print("text into #2: %s %s" % (port2["kind"], port2["lines"]))
    ok &= port2["kind"] == "unknown" and port2["lines"] == ["SOME DEVICE TALKING ", "LINE TWO"]

    for i in range(10):
        os.write(p2_m, bno(i))
        time.sleep(0.01)
    time.sleep(0.2)
    st = status()
    print("BNO into #2: %s; imu %s" % (port(st, "Port 2")["kind"], st["imu"]))
    ok &= port(st, "Port 2")["kind"] == "imu" and st["imu"]["type"] == "BNO085"

    # The radio unplugged, a receiver on #1; the onboard one silent. Lost after 5 s, then found.
    deadline = time.time() + 9
    found = False
    while time.time() < deadline and not found:
        os.write(p1_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % time.strftime("%H%M%S.00")))
        time.sleep(0.2)
        st = status()
        found = st["serial"]["gnssPort"] == os.ttyname(p1_s)
    print("receiver on #1: %s, in use %s, fix %s" % (port(st, "Port 1")["kind"], st["serial"]["gnssPort"], st["gnss"]["fixQuality"]))
    ok &= found and port(st, "Port 1")["kind"] == "gnss" and st["gnss"]["fixQuality"] == 4

    proc.terminate()
    proc.wait(timeout=5)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
