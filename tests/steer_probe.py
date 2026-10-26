#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""Closed-loop steering test against aow-hatd running with --board sim.

Plays the app: sends steer settings (252) and config (251), engages at 5 km/h, asks for a
wheel angle and follows PGN 253 as the simulated wheel gets there. Then goes quiet and checks
the daemon logs the 2 s communication timeout, and that the app's gains were saved.

Stop AgOpenWeb first: this binds the app's port (9999).

    python3 tests/steer_probe.py --web 80 --settings /etc/aow-hat/settings.json
"""
import argparse
import json
import socket
import struct
import sys
import time
import urllib.request


def frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def steer_data(speed_kmh, guidance, target_deg):
    return frame(254, struct.pack("<HBhbBB", int(speed_kmh * 10), 0x01 if guidance else 0, int(target_deg * 100), 0, 0, 0))


def parse_253(b):
    if len(b) == 14 and b[:2] == b"\x80\x81" and b[3] == 253:
        angle = struct.unpack("<h", b[5:7])[0] / 100.0
        return angle, b[11], b[12]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--web", type=int, default=80, help="daemon web port, for its log")
    ap.add_argument("--settings", help="settings file, to check the app's gains were saved")
    ap.add_argument("--target", type=float, default=5.0)
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9999))
    s.settimeout(0.02)
    dest = (a.host, 8888)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def latest_253():
        last = None
        try:
            while True:
                p = parse_253(s.recv(64))
                if p:
                    last = p
        except socket.timeout:
            pass
        return last

    # Gains: Kp (below), high 255, low 0, min 10, 30 counts/deg, offset 0, Ackermann 1.00.
    # A value that differs from run to run, to see it saved, and never the default (40): the daemon
    # saves only what changed, so a fresh settings file would not be written at all.
    kp = 41 + int(time.time()) % 19
    s.sendto(frame(252, [kp, 255, 0, 10, 30, 0, 0, 100]), dest)
    # Config: nothing inverted, no switch or button, min speed 0.3 km/h, no current sensor.
    s.sendto(frame(251, [0, 0, 3, 0, 0, 0, 0, 0]), dest)

    # Guidance off for a moment, then on: the app's steer button arms the module.
    for _ in range(20):
        s.sendto(steer_data(5.0, False, 0), dest)
        time.sleep(0.01)
    t0 = time.time()
    track = []
    while time.time() - t0 < 4.0:
        s.sendto(steer_data(5.0, True, a.target), dest)
        p = latest_253()
        if p:
            track.append((time.time() - t0, *p))
    armed = [t for t in track if not (t[2] & 0x02)]
    check(armed, "armed by guidance on (PGN 253 steer bit)")
    first_pwm = next((t for t in track if t[3] > 0), None)
    check(first_pwm and first_pwm[0] < 0.1, "motor running within 100 ms, first PWM %s" % (first_pwm and first_pwm[3]))
    reached = next((t for t in track if abs(t[1] - a.target) < 0.2), None)
    final = track[-1] if track else None
    check(reached, "wheel reached %.1f deg after %.2f s" % (a.target, reached[0] if reached else -1))
    check(final and abs(final[1] - a.target) < 0.2, "holding: actual %.2f deg, PWM %d" % (final[1], final[3]) if final else "no 253")

    # The app goes quiet: the daemon must log the communication timeout after 2 s.
    time.sleep(2.5)
    with urllib.request.urlopen("http://%s:%d/api/logs/data" % (a.host, a.web), timeout=3) as r:
        logs = [e["message"] for e in json.load(r)["logs"]]
    check(any("communication timeout" in m for m in logs[-20:]), "logged 'Motor disabled - communication timeout'")

    if a.settings:
        time.sleep(0.2)
        with open(a.settings) as f:
            saved = json.load(f)
        check(saved["gains"]["kp"] == kp, "settings file has Kp %d from PGN 252" % kp)

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
