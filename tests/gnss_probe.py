#!/usr/bin/env python3
"""End-to-end GNSS test with no receiver: a pseudo-terminal stands in for it.

Starts aow-hatd with --gnss on the pty, plays GGA/VTG (then HPR) sentences into it, and
checks that PANDA (then PAOGI) arrives on the app's port with good checksums. Then sends
RTCM to the daemon's RTCM port and checks it comes out of the "receiver" side unchanged.

Stop AgOpenWeb first: this binds the app's port (9999).

    python3 tests/gnss_probe.py build/aow-hatd
"""
import functools
import os
import pty
import select
import socket
import subprocess
import sys
import time
import tty


def nmea(body):
    cs = functools.reduce(lambda a, c: a ^ ord(c), body, 0)
    return ("$%s*%02X\r\n" % (body, cs)).encode()


def checksum_ok(line):
    body, _, tail = line.partition("*")
    cs = functools.reduce(lambda a, c: a ^ ord(c), body[1:], 0)
    return tail.strip() == "%02X" % cs


def hhmmss(t):
    return time.strftime("%H%M%S", time.gmtime(t)) + ".%02d" % int((t % 1) * 100)


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    master, slave = pty.openpty()
    tty.setraw(master)
    slave_name = os.ttyname(slave)

    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    app.bind(("0.0.0.0", 9999))
    app.settimeout(0.5)

    config = "/tmp/gnss_probe_settings.json"
    proc = subprocess.Popen([daemon, "--gnss", slave_name, "--gnss-baud", "460800", "--web-port", "0",
                             "--config", config])
    time.sleep(0.3)
    ok = True

    def play(extra, count=20, rate=10.0):
        got = []
        for _ in range(count):
            t = time.time()
            os.write(master, nmea("GNVTG,054.7,T,034.4,M,005.5,N,010.2,K,D"))
            os.write(master, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000"
                                  % hhmmss(t)))
            for e in extra:
                os.write(master, nmea(e % hhmmss(t)))
            try:
                while True:
                    app.settimeout(0.05)
                    b = app.recv(512)
                    if b.startswith(b"$"):  # NMEA only; PGN 250 etc. are binary
                        got.append(b.decode())
            except socket.timeout:
                pass
            time.sleep(max(0.0, 1 / rate - (time.time() - t)))
        return got

    got = play([])
    panda = [g for g in got if g.startswith("$PANDA")]
    print("single antenna: %d PANDA for 20 GGA; first: %s" % (len(panda), panda[0].strip() if panda else "-"))
    ok &= len(panda) == 20 and all(checksum_ok(g) for g in panda) and ",4,24,0.6," in panda[0]

    got = play(["GNHPR,%s,271.25,-1.50,0.30,4,24,1.0,0000"])
    paogi = [g for g in got if g.startswith("$PAOGI")]
    print("dual antenna:   %d PAOGI; first: %s" % (len(paogi), paogi[0].strip() if paogi else "-"))
    ok &= len(paogi) >= 19 and all(checksum_ok(g) for g in paogi) and ",-1.50," in paogi[0]

    rtcm = bytes([0xD3, 0x00, 0x13]) + bytes(range(19)) + bytes([0x11, 0x22, 0x33])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(rtcm, ("127.0.0.1", 2233))
    received = b""
    deadline = time.time() + 1.0
    while len(received) < len(rtcm) and time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.1)
        if r:
            received += os.read(master, 1024)
    print("rtcm:           %d bytes in, %d out, %s" % (len(rtcm), len(received),
                                                        "identical" if received == rtcm else "DIFFERENT"))
    ok &= received == rtcm

    proc.terminate()
    proc.wait(timeout=2)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
