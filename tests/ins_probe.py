#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""The UM981's INS lever arm from AgOpenWeb's vehicle geometry (PGN 247), end to end, with a
pseudo-terminal as the receiver.

The fake UM981 streams INSPVAXA at 10 Hz and answers UNILOGLIST, UNLOGALL, CONFIG, CONFIG
IMUTOANT, CONFIG INSSOL and SAVECONFIG, keeping what it is set to. The test plays the app:
it sends PGN 247 and checks the receiver is set (logs back on before SAVECONFIG), that the same
geometry again changes nothing, and that a new one waits while the vehicle moves.

    python3 tests/ins_probe.py build/aow-hatd

Stop AgOpenWeb first: this binds the app's port (9999).
"""
import json
import os
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import urllib.request

WEB = 8084


def crc32(s):  # Unicore's: CRC-32 from 0, no final XOR (aow::nmea::crc32)
    crc = 0
    for c in s.encode():
        crc ^= c
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc


def frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def geometry(antenna, imu, imu_entered=True):
    """PGN 247 as AgOpenWeb builds it: positions (pivot, offset, height) in cm."""
    (ap, ao, ah), (ip, io, ih) = antenna, imu
    return frame(247, [1, 1 if imu_entered else 0] + list(struct.pack("<HHHhhhHhB", 250, 180, ah, ap, ao, ip, ih, io, 45)))


class FakeUm981(threading.Thread):
    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd, self.stop, self.streaming, self.commands = fd, False, True, []
        self.logs = ["INSPVAXA COM1 0.1"]
        self.imutoant, self.inssol = "0.00 0.00 0.00 0.01 0.01 0.01", "0.00 0.00 0.00"
        self.saved = None
        self.velocity = (0.0, 0.0)  # north, east m/s

    def reply(self, cmd):
        out = ["$command,%s,response: OK*00" % cmd]
        if cmd == "CONFIG":
            out += ["$CONFIG,INS,CONFIG INS ANGLE 0 0 0*00",
                    "$CONFIG,IMUTOANT,CONFIG IMUTOANT OFFSET %s*00" % self.imutoant,
                    "$CONFIG,INSSOL,CONFIG INSSOL OFFSET %s*00" % self.inssol,
                    "$CONFIG,COM1,CONFIG COM1 460800*00", "$CONFIG,COM3,CONFIG COM3 115200*00"]
        elif cmd == "UNILOGLIST":
            out += ["<\t%d" % len(self.logs)] + ["<\t" + l for l in self.logs]
        elif cmd.startswith("UNLOGALL"):
            self.streaming = False
        elif cmd.startswith("INSPVAXA"):
            self.streaming = True
        elif cmd.startswith("CONFIG IMUTOANT OFFSET "):
            self.imutoant = cmd[len("CONFIG IMUTOANT OFFSET "):]
        elif cmd.startswith("CONFIG INSSOL OFFSET "):
            self.inssol = cmd[len("CONFIG INSSOL OFFSET "):]
        elif cmd == "SAVECONFIG":
            self.saved = (self.imutoant, self.inssol, self.streaming)
        os.write(self.fd, "".join(l + "\r\n" for l in out).encode())

    def run(self):
        buf, last = b"", 0.0
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                buf += os.read(self.fd, 4096)
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                    self.commands.append(line.decode(errors="replace"))
                    self.reply(self.commands[-1])
            if self.streaming and time.time() - last > 0.1:
                last = time.time()
                week_s = (time.time() - 315964800 + 18) % 604800
                vn, ve = self.velocity
                body = ("INSPVAXA,COM1,0,55.5,FINESTEERING,2400,%.3f,0,0,0;INS_SOLUTION_GOOD,INS_RTKFIXED,"
                        "48.1173,11.516667,545.4,46.9,%.3f,%.3f,0.0,1.25,-0.50,181.75,"
                        "0.01,0.01,0.02,0.01,0.01,0.01,0.1,0.1,0.2,0,1" % (week_s, vn, ve))
                os.write(self.fd, ("#%s*%08x\r\n" % (body, crc32(body))).encode())


def status():
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/api/status" % WEB, timeout=10) as r:
            return json.load(r)
    except OSError:  # not listening yet
        return {}


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    gm, gs = pty.openpty()
    tty.setraw(gm)
    rx = FakeUm981(gm)
    rx.start()

    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    app.bind(("0.0.0.0", 9999))
    app.settimeout(0.2)
    cfg = os.path.join(tempfile.mkdtemp(), "settings.json")
    proc = subprocess.Popen([daemon, "--gnss", os.ttyname(gs), "--config", cfg, "--web-port", str(WEB),
                             "--gvret-port", "0"], stderr=subprocess.DEVNULL)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def wait_for(pred, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.1)
        return False

    def paogi(seconds):
        n, end = 0, time.time() + seconds
        while time.time() < end:
            try:
                if app.recv(512).startswith(b"$PAOGI"):
                    n += 1
            except socket.timeout:
                pass
        return n

    try:
        check(wait_for(lambda: status().get("gnssIns"), 5), "the daemon sees an INS receiver")
        check(paogi(1.0) >= 8, "PAOGI flowing")

        # Antenna 3.00 m high, 1.20 ahead, 0.15 left; the receiver 1.50 high, 0.40 ahead, 0.25 right.
        rx.commands.clear()
        app.sendto(geometry((120, -15, 300), (40, 25, 150)), ("127.0.0.1", 8888))
        check(wait_for(lambda: rx.saved is not None, 8), "receiver set and saved")
        want = ["UNILOGLIST", "UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3", "CONFIG",
                "CONFIG IMUTOANT OFFSET -0.40 0.80 1.50 0.03 0.03 0.03", "CONFIG INSSOL OFFSET -0.40 0.80 1.50",
                "INSPVAXA COM1 0.1", "SAVECONFIG"]
        check(rx.commands == want, "commands in order, logs back on before SAVECONFIG: %s" % rx.commands)
        check(rx.saved == ("-0.40 0.80 1.50 0.03 0.03 0.03", "-0.40 0.80 1.50", True), "saved with the logs on: %s" % (rx.saved,))
        msg = status().get("insLeverArm", "")
        check("saved on the receiver" in msg, "status: %s" % msg)
        check(paogi(1.0) >= 8, "PAOGI flowing again")

        # The app sends the same geometry again (it does, often): the receiver is left alone.
        rx.commands.clear()
        app.sendto(geometry((120, -15, 300), (40, 25, 150)), ("127.0.0.1", 8888))
        time.sleep(2.5)
        check(rx.commands == [], "same geometry again: receiver not touched")

        # A new antenna position while moving at 18 km/h: it waits; stopped, it is set.
        rx.velocity = (3.0, 4.0)
        time.sleep(0.5)
        app.sendto(geometry((130, -15, 300), (40, 25, 150)), ("127.0.0.1", 8888))
        time.sleep(2.5)
        msg = status().get("insLeverArm", "")
        check(rx.commands == [] and "waiting" in msg, "moving: waits (%s)" % msg)
        rx.velocity = (0.0, 0.0)
        check(wait_for(lambda: "SAVECONFIG" in rx.commands, 8), "stopped: set")
        check(rx.imutoant == "-0.40 0.90 1.50 0.03 0.03 0.03" and rx.inssol == "-0.40 0.90 1.50",
              "receiver now %s / %s" % (rx.imutoant, rx.inssol))

        # No IMU position entered in the app: nothing touched, and the status says why.
        rx.commands.clear()
        app.sendto(geometry((130, -15, 300), (0, 0, 0), imu_entered=False), ("127.0.0.1", 8888))
        time.sleep(2.5)
        msg = status().get("insLeverArm", "")
        check(rx.commands == [] and "No IMU position" in msg, "no IMU position: left alone (%s)" % msg)
    finally:
        rx.stop = True
        proc.terminate()
        proc.wait(timeout=3)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
