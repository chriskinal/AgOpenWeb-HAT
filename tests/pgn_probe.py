#!/usr/bin/env python3
"""Stand in for the app against a running aow-hatd: send hello, scan and PGN 254 frames
and check the replies.

Stop AgOpenWeb first: this binds the app's port (9999).

    python3 tests/pgn_probe.py                  daemon on this machine
    python3 tests/pgn_probe.py --rate 100 -n 500   100 Hz of PGN 254, like the app
"""
import argparse
import socket
import struct
import sys
import time


def frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


HELLO = bytes([0x80, 0x81, 0x7F, 0xC8, 0x03, 0x38, 0x00, 0x00, 0x47])
SCAN = bytes([0x80, 0x81, 0x7F, 0xCA, 0x03, 0xCA, 0xCA, 0x05, 0x47])


def steer_data(speed_kmh=5.0, target_deg=0.0, status=0x00):
    return frame(254, struct.pack("<HBhbBB", int(speed_kmh * 10), status, int(target_deg * 100), 0, 0, 0))


def receive(sock, timeout):
    sock.settimeout(timeout)
    out = []
    try:
        while True:
            out.append(sock.recv(512))
            sock.settimeout(0.02)
    except socket.timeout:
        pass
    return out


def wait_for_253(sock, deadline):
    while (left := deadline - time.monotonic()) > 0:
        sock.settimeout(left)
        try:
            b = sock.recv(512)
        except socket.timeout:
            return False
        if check_frame(b) and b[3] == 253:
            return True
    return False


def check_frame(b):
    if len(b) < 6 or b[0:2] != b"\x80\x81" or len(b) != 6 + b[4]:
        return False
    return sum(b[2:-1]) & 0xFF == b[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1", help="daemon address")
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--rate", type=float, default=10.0, help="PGN 254 per second")
    ap.add_argument("-n", type=int, default=20, help="PGN 254 frames to send")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9999))
    dest = (a.host, a.port)
    ok = True

    s.sendto(HELLO, dest)
    got = sorted((r[2], r[3]) for r in receive(s, 0.5) if check_frame(r))
    print("hello  ->", got)
    ok &= got == [(120, 120), (126, 126)]

    s.sendto(SCAN, dest)
    replies = [r for r in receive(s, 0.5) if check_frame(r) and r[3] == 203]
    for r in replies:
        print("scan   -> src %d ip %d.%d.%d.%d" % (r[2], *r[5:9]))
    ok &= len(replies) == 2

    period = 1.0 / a.rate
    lat = []
    missing = 0
    for i in range(a.n):
        t0 = time.monotonic()
        s.sendto(steer_data(target_deg=(i % 20) - 10), dest)
        if wait_for_253(s, t0 + 0.1):
            lat.append((time.monotonic() - t0) * 1000)
        else:
            missing += 1
        time.sleep(max(0.0, period - (time.monotonic() - t0)))
    if lat:
        lat.sort()
        print("pgn254 -> %d/%d answered by 253; round trip median %.2f ms, worst %.2f ms"
              % (len(lat), a.n, lat[len(lat) // 2], lat[-1]))
    ok &= missing == 0

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
