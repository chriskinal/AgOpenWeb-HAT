#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""UM98x setup page and RTK radio end to end, with pseudo-terminals as the receiver and radio.

The fake UM982 streams GGA at 10 Hz and answers UNLOGALL, CONFIG, MODE, UNILOGLIST and
SAVECONFIG like the real one. The test reads and writes its setup through the web API while
the daemon runs, then checks PANDA resumes and radio bytes reach the receiver.

    python3 tests/um98x_probe.py build/aow-hatd

Stop AgOpenWeb first: this binds the app's port (9999).
"""
import functools
import json
import os
import pty
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty
import urllib.request

WEB = 8081


def nmea(body):
    cs = functools.reduce(lambda a, c: a ^ ord(c), body, 0)
    return ("$%s*%02X\r\n" % (body, cs)).encode()


class FakeUm982(threading.Thread):
    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd, self.stop, self.streaming, self.commands, self.radio = fd, False, True, [], b""
        self.logs = ["GPGGA COM1 0.1"]

    def reply(self, cmd):
        out = ["$command,%s,response: OK*00" % cmd]
        if cmd == "CONFIG":
            out += ["$CONFIG,COM1,CONFIG COM1 460800*33", "$CONFIG,COM3,CONFIG COM3 115200*31"]
        elif cmd == "MODE":
            out += ["#MODE,97,GPS,FINE,2190,1,0,0;MODE ROVER SURVEY,*1a"]
        elif cmd == "UNILOGLIST":
            out += ["<\t%d" % len(self.logs)] + ["<\t" + l for l in self.logs]
        elif cmd.startswith("UNLOGALL"):
            self.streaming = False
        elif cmd.startswith("GPGGA"):
            self.streaming = True
        os.write(self.fd, "".join(l + "\r\n" for l in out).encode())

    def run(self):
        buf, last = b"", 0.0
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                data = os.read(self.fd, 4096)
                buf += data
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                    if line.startswith(b"\xd3"):  # RTCM from the radio, not a command
                        self.radio += line
                        continue
                    self.commands.append(line.decode(errors="replace"))
                    self.reply(self.commands[-1])
                if buf.startswith(b"\xd3"):
                    self.radio, buf = self.radio + buf, b""
            if self.streaming and time.time() - last > 0.1:
                last = time.time()
                t = time.strftime("%H%M%S.00", time.gmtime())
                os.write(self.fd, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % t))


def api(path, body=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (WEB, path), data=body and json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    gm, gs = pty.openpty()
    rm, rs = pty.openpty()
    tty.setraw(gm)
    tty.setraw(rm)
    rx = FakeUm982(gm)
    rx.start()

    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    app.bind(("0.0.0.0", 9999))
    app.settimeout(0.2)
    cfg = os.path.join(tempfile.mkdtemp(), "settings.json")
    proc = subprocess.Popen([daemon, "--gnss", os.ttyname(gs), "--radio", os.ttyname(rs), "--config", cfg,
                             "--web-port", str(WEB), "--gvret-port", "0"], stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def pandas(seconds):
        n, end = 0, time.time() + seconds
        while time.time() < end:
            try:
                if app.recv(512).startswith(b"$PANDA"):
                    n += 1
            except socket.timeout:
                pass
        return n

    check(pandas(1.0) >= 8, "PANDA flowing before setup")

    t0 = time.time()
    got = api("/api/um98x/read")
    check(got.get("success") and got["config"] == "CONFIG COM1 460800\nCONFIG COM3 115200"
          and got["mode"] == "MODE ROVER SURVEY" and got["messages"] == "GPGGA COM1 0.1",
          "read config in %.1f s: %s" % (time.time() - t0, json.dumps(got)[:120]))
    check(pandas(1.0) >= 8, "PANDA flowing again after the read")

    rx.commands.clear()
    got = api("/api/um98x/write", {"config": "CONFIG COM1 460800", "mode": "MODE ROVER SURVEY",
                                   "messages": "GPGGA COM1 0.1"})
    check(got.get("success"), "write config: %s" % got)
    check(rx.commands and rx.commands[-1] == "SAVECONFIG", "SAVECONFIG sent last (%d commands)" % len(rx.commands))
    check(pandas(1.0) >= 8, "PANDA flowing again after the write")

    rtcm = b"\xd3\x00\x13" + bytes(range(19)) + b"\x11\x22\x33"
    os.write(rm, rtcm)
    time.sleep(0.3)
    check(rx.radio == rtcm, "radio RTCM reached the receiver byte for byte (%d bytes)" % len(rx.radio))

    rx.stop = True
    proc.terminate()
    proc.wait(timeout=3)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
