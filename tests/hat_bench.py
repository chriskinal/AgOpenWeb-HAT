#!/usr/bin/env python3
"""Bench driver for aow-hatd --board hat with a pot as the WAS and a meter on the motor outputs.

Plays the app: sends gains (252) and config (251), then PGN 254 at 100 Hz with guidance on and
a fixed target angle, for --seconds. Once a second prints what the daemon reports in PGN 253:
the wheel angle it reads from the pot, the PWM it is commanding, and the switch byte. Turn the
pot through the target and the PWM should fall to zero and reverse (DIR on J3.11 flips).

Stop AgOpenWeb first: this binds the app's port (9999).

    python3 tests/hat_bench.py --target 20 --seconds 180
"""
import argparse
import socket
import struct
import sys
import time


def frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def steer_data(speed_kmh, guidance, target_deg):
    return frame(254, struct.pack("<HBhbBB", int(speed_kmh * 10), 0x01 if guidance else 0, int(target_deg * 100), 0, 0, 0))


def parse_253(b):
    if len(b) == 14 and b[:2] == b"\x80\x81" and b[3] == 253:
        return struct.unpack("<h", b[5:7])[0] / 100.0, b[11], b[12]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--target", type=float, default=20.0)
    ap.add_argument("--seconds", type=float, default=180)
    ap.add_argument("--kp", type=int, default=40)
    ap.add_argument("--speed", type=float, default=5.0)
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9999))
    s.setblocking(False)
    dest = (a.host, 8888)

    # Gains: Kp, high 255, low 0, min 10, 30 counts/deg, offset 0, Ackermann 1.00.
    s.sendto(frame(252, [a.kp, 255, 0, 10, 30, 0, 0, 100]), dest)
    # Config: nothing inverted, no switch or button, min speed 0.3 km/h, no current sensor.
    s.sendto(frame(251, [0, 0, 3, 0, 0, 0, 0, 0]), dest)
    for _ in range(20):
        s.sendto(steer_data(a.speed, False, 0), dest)
        time.sleep(0.01)

    t0 = time.time()
    next_print = t0
    last = None
    n = 0
    print("target %.1f deg for %.0f s; angle / pwm / switch byte once a second" % (a.target, a.seconds), flush=True)
    while time.time() - t0 < a.seconds:
        s.sendto(steer_data(a.speed, True, a.target), dest)
        try:
            while True:
                p = parse_253(s.recv(64))
                if p:
                    last = p
                    n += 1
        except BlockingIOError:
            pass
        now = time.time()
        if now >= next_print:
            next_print += 1.0
            if last:
                angle, sw, pwm = last
                print("%5.0f s  angle %6.2f  err %6.2f  pwm %3d  switch 0x%02x  armed %s  (253: %d)" %
                      (now - t0, angle, a.target - angle, pwm, sw, "yes" if not (sw & 0x02) else "no", n), flush=True)
            else:
                print("%5.0f s  no PGN 253 yet" % (now - t0), flush=True)
        time.sleep(0.01)
    for _ in range(10):
        s.sendto(steer_data(a.speed, False, 0), dest)
        time.sleep(0.01)
    print("done: guidance off", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
