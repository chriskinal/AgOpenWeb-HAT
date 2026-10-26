#!/usr/bin/env python3
"""End-to-end external IMU test with no hardware: pseudo-terminals stand in for the GNSS
receiver and for a BNO085 in RVC mode.

Starts aow-hatd with --gnss and --imu on the ptys, sends hello (no IMU yet: no 121 reply),
then streams RVC packets turning at +10 deg/s at 100 Hz alongside GGA at 10 Hz. Checks that
hello is then answered as the IMU (121) and that PANDA carries the IMU heading, roll, pitch
and a yaw rate near 10 deg/s. Then stops the IMU and checks PANDA goes back to "no IMU".
Last, it unplugs the IMU (closes the pty, as a USB IMU vanishing does) and plugs a new one in
under the same name (a symlink, as /dev/serial/by-id is), and checks the daemon reopens it.

Stop AgOpenWeb first: this binds the app's port (9999).

    python3 tests/imu_probe.py build/aow-hatd
"""
import functools
import os
import pty
import socket
import struct
import subprocess
import sys
import time
import tty

HELLO = bytes([0x80, 0x81, 0x7F, 0xC8, 0x03, 0x38, 0x00, 0x00, 0x47])


def nmea(body):
    cs = functools.reduce(lambda a, c: a ^ ord(c), body, 0)
    return ("$%s*%02X\r\n" % (body, cs)).encode()


def rvc(index, yaw, pitch, roll):
    body = struct.pack("<Bhhh9x", index & 0xFF, round(yaw * 100), round(pitch * 100), round(roll * 100))
    return b"\xAA\xAA" + body + bytes([sum(body) & 0xFF])


def hhmmss(t):
    return time.strftime("%H%M%S", time.gmtime(t)) + ".%02d" % int((t % 1) * 100)


def drain(app, wait=0.02):
    """Everything that arrives within `wait` seconds. A deadline, not a quiet gap: the daemon
    sends PGN 250 every 100 ms whatever else happens."""
    got = []
    end = time.time() + wait
    while True:
        left = end - time.time()
        if left <= 0:
            return got
        app.settimeout(left)
        try:
            got.append(app.recv(512))
        except socket.timeout:
            return got


def hello_sources(app):
    app.sendto(HELLO, ("127.0.0.1", 8888))
    time.sleep(0.05)
    return sorted(b[2] for b in drain(app, 0.1) if len(b) > 4 and b[:2] == b"\x80\x81" and b[3] == b[2])


def new_imu(link):
    m, s = pty.openpty()
    tty.setraw(m)
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(os.ttyname(s), link)
    return m, s


def stream(gps_m, imu_m, app, seconds, yaw):
    """IMU at 100 Hz turning at +10 deg/s, GGA at 10 Hz. Returns the PANDA seen and the yaw."""
    panda = []
    start = time.time()
    for i in range(int(seconds * 100)):
        t = time.time()
        sent = yaw - 360.0 if yaw > 180.0 else yaw
        os.write(imu_m, rvc(i, sent, 1.5, -2.25))
        yaw = (yaw + 0.1) % 360.0
        if i % 10 == 0:
            os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % hhmmss(t)))
        panda += [b.decode() for b in drain(app, 0.001) if b.startswith(b"$PANDA")]
        time.sleep(max(0.0, start + (i + 1) * 0.01 - time.time()))
    return panda, yaw


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    gps_m, gps_s = pty.openpty()
    tty.setraw(gps_m)
    link = "/tmp/imu_probe_imu"
    imu_m, imu_s = new_imu(link)

    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    app.bind(("0.0.0.0", 9999))

    proc = subprocess.Popen([daemon, "--gnss", os.ttyname(gps_s), "--imu", link, "--web-port", "0",
                             "--config", "/tmp/imu_probe_settings.json"])
    time.sleep(0.3)
    ok = True

    src = hello_sources(app)
    print("hello before the IMU: sources %s" % src)
    ok &= src == [120, 126]

    # 2 s turning at +10 deg/s through north.
    panda, yaw = stream(gps_m, imu_m, app, 2.0, 355.0)

    src = hello_sources(app)
    print("hello with the IMU: sources %s" % src)
    ok &= src == [120, 121, 126]

    last = panda[-1].split("*")[0].split(",") if panda else []
    print("%d PANDA; last IMU fields: %s" % (len(panda), last[12:16]))
    if len(last) >= 16:
        heading, roll, pitch, rate = int(last[12]), int(last[13]), int(last[14]), float(last[15])
        # PANDA goes out on GGA, sent with every 10th IMU packet: the last one carries a heading
        # from the packets around the last GGA, 0 to 1 deg behind the last one sent.
        behind = ((yaw - 0.1) - heading / 10.0 + 180.0) % 360.0 - 180.0
        ok &= 0 <= heading < 3600 and -0.15 < behind < 1.15
        ok &= roll == -22 or roll == -23  # -2.25 deg x10
        ok &= pitch in (1, 2)             # degrees
        ok &= abs(rate - 10.0) < 1.0
    else:
        ok = False

    # IMU stops: PANDA goes back to "no IMU" within a fix or two.
    time.sleep(0.2)
    for _ in range(3):
        os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % hhmmss(time.time())))
        time.sleep(0.1)
    after = [b.decode() for b in drain(app, 0.1) if b.startswith(b"$PANDA")]
    print("after the IMU stops: %s" % (after[-1].strip() if after else "-"))
    ok &= bool(after) and ",65535,0,0,0*" in after[-1]

    # Unplugged, then a new one under the same name: reopened within about a second.
    os.close(imu_m)
    os.close(imu_s)
    time.sleep(0.3)
    imu_m, imu_s = new_imu(link)
    time.sleep(1.5)
    panda, yaw = stream(gps_m, imu_m, app, 1.0, yaw)
    back = [p for p in panda if ",65535," not in p]
    print("after unplug and replug: %d of %d PANDA carry the IMU" % (len(back), len(panda)))
    ok &= len(back) >= 5

    proc.terminate()
    proc.wait(timeout=5)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
