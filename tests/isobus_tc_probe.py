#!/usr/bin/env python3
"""ISOBUS section control end to end on virtual CAN: aow-hatd as the task controller on CAN2
(vcan1), a simulated implement beside it, and this script as the app.

    sudo modprobe vcan; for i in 0 1 2; do sudo ip link add vcan$i type vcan; sudo ip link set vcan$i up; done
    python3 tests/isobus_tc_probe.py build/aow-hatd build/isobus_implement

Uses its own UDP ports, so AgOpenWeb can stay up; stop the aow-hatd service (WebSocket ports).
"""
import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

WEB_PORT = 8089
DAEMON_PORT = 18888
APP_PORT = 19999


def pgn(num, data, src=0x7F):
    body = bytes([src, num, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def bits(n, count):
    """Section bits as the implement and the status page print them: section 1 first."""
    return "".join("1" if (n >> i) & 1 else "0" for i in range(count))


class App:
    """Sends what AgOpenWeb sends: PGN 254 with 16 section bits, PGN 229 with 64 when asked."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", APP_PORT))
        self.sock.setblocking(False)
        self.sections = 0
        self.send64 = False
        self.silent = False
        self.status240 = None  # the last PGN 240: (section control, count, bits as an integer)
        self.count240 = 0

    def tick(self):
        try:
            while True:
                d = self.sock.recv(2048)
                if len(d) >= 8 and d[:2] == b"\x80\x81" and d[3] == 240 and sum(d[2:-1]) & 0xFF == d[-1]:
                    self.status240 = (d[2], d[5], d[6], int.from_bytes(d[7:-1], "little"), d[4])
                    self.count240 += 1
        except BlockingIOError:
            pass
        if self.silent:
            return
        to = ("127.0.0.1", DAEMON_PORT)
        s = self.sections
        self.sock.sendto(pgn(254, struct.pack("<HBhbBB", 50, 0, 0, 0, s & 0xFF, (s >> 8) & 0xFF)), to)
        if self.send64:
            self.sock.sendto(pgn(229, struct.pack("<Q", s) + bytes([50, 50])), to)

    def send(self, frame):
        self.sock.sendto(frame, ("127.0.0.1", DAEMON_PORT))


class Implement:
    def __init__(self, binary, count):
        self.count = count
        self.proc = subprocess.Popen([binary, "vcan1", str(count)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.buf = b""
        self.sections = "0" * count
        self.connected = False

    def poll(self):
        if select.select([self.proc.stdout], [], [], 0)[0]:
            self.buf += self.proc.stdout.read() or b""
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            key, _, value = line.decode().partition(" ")
            if key == "sections":
                self.sections = value
            elif key == "connected":
                self.connected = value == "1"

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)


def status():
    with urllib.request.urlopen("http://127.0.0.1:%d/api/status" % WEB_PORT, timeout=2) as r:
        return json.load(r)["isobus"]


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    implement_bin = sys.argv[2] if len(sys.argv) > 2 else "build/isobus_implement"
    cfgdir = tempfile.mkdtemp()
    settings = os.path.join(cfgdir, "settings.json")
    with open(settings, "w") as f:
        json.dump({"isobus": {"enabled": True, "channel": 2}}, f)

    app = App()
    proc = subprocess.Popen([daemon, "--config", settings, "--web-port", str(WEB_PORT), "--can", "vcan0,vcan1,vcan2",
                             "--gvret-port", "2323", "--listen-port", str(DAEMON_PORT), "--app-port", str(APP_PORT)],
                            stderr=open(os.path.join(cfgdir, "daemon.log"), "w"))
    ok = True
    imp = None

    def check(cond, what):
        nonlocal ok
        ok = ok and bool(cond)
        print(("ok    " if cond else "FAIL  ") + what)

    def run(seconds, until=None):
        """Plays the app at 50 Hz. Returns the time until the condition held, or None."""
        start = time.time()
        while time.time() - start < seconds:
            app.tick()
            if imp:
                imp.poll()
            if until and until():
                return time.time() - start
            time.sleep(0.02)
        return None if until else seconds

    try:
        # 1. The implement finds the task controller, uploads its description and is accepted.
        run(1.0)
        check(app.status240 == (0x80, 0, 0, 0, 2), "PGN 240 heartbeat with no implement: %s" % (app.status240,))
        n = app.count240
        run(1.0)
        check(8 <= app.count240 - n <= 12, "PGN 240 comes at 10 Hz (%d in 1 s)" % (app.count240 - n))
        imp = Implement(implement_bin, 16)
        t = run(30.0, lambda: imp.connected and status()["taskController"]["sections"] == 16)
        check(t is not None, "a 16-section implement connects (%s s)" % ("%.1f" % t if t else "never"))
        tc = status()["taskController"]
        check(tc["clients"] >= 1 and tc["width"] > 0 and tc["implement"] == "AgIsoStack++ UnitTest", "status names it: %s" % tc)
        check(os.path.exists(os.path.join(cfgdir, "isobus", "tc.log")), "the task controller's log file is written")
        saved = [f for _, _, fs in os.walk(os.path.join(cfgdir, "isobus")) for f in fs if f.endswith(".ddop")]
        check(saved, "the implement's description is saved (%d files)" % len(saved))

        # 2. Sections from PGN 254 reach the implement, and its report comes back.
        app.sections = 0x80A5
        t = run(3.0, lambda: imp.sections == bits(0x80A5, 16))
        check(t is not None, "PGN 254 sections reach the implement (%s)" % ("%.0f ms" % (t * 1000) if t else imp.sections))
        run(4.0, lambda: status()["taskController"]["actual"] == bits(0x80A5, 16))
        tc = status()["taskController"]
        check(tc["setpoint"] == bits(0x80A5, 16) and tc["actual"] == bits(0x80A5, 16) and tc["sectionControl"],
              "status shows setpoint and the implement's actual: %s / %s" % (tc["setpoint"], tc["actual"]))
        t = run(1.0, lambda: app.status240 == (0x80, 1, 16, 0x80A5, 4))
        check(t is not None, "PGN 240 carries the implement's 16 sections as it reports them: %s" % (app.status240,))
        app.sections = 0x0001
        t = run(3.0, lambda: imp.sections == bits(0x0001, 16))
        check(t is not None, "a change follows (%s)" % ("%.0f ms" % (t * 1000) if t else imp.sections))

        # 3. PGN 241 from an app that has it: off drops every section, on brings them back.
        app.sections = 0x00FF
        run(3.0, lambda: imp.sections == bits(0x00FF, 16))
        app.send(pgn(241, [0]))
        t = run(3.0, lambda: imp.sections == bits(0, 16))
        check(t is not None, "PGN 241 off: all sections off")
        t = run(1.0, lambda: not status()["taskController"]["sectionControl"])
        check(t is not None, "status shows section control off")
        t = run(4.0, lambda: app.status240 == (0x80, 0, 16, 0, 4))
        check(t is not None, "PGN 240 shows section control off and no sections on: %s" % (app.status240,))
        app.send(pgn(241, [1]))
        t = run(3.0, lambda: imp.sections == bits(0x00FF, 16))
        check(t is not None, "PGN 241 on: sections back")

        # 4. The app goes away: sections off after the 2 s app timeout.
        app.silent = True
        t = run(5.0, lambda: imp.sections == bits(0, 16))
        check(t is not None and 1.5 < t < 3.0, "app silent: all sections off after %s" % ("%.1f s" % t if t else "never"))
        app.silent = False
        t = run(3.0, lambda: imp.sections == bits(0x00FF, 16))
        check(t is not None, "app back: sections back")

        # 5. The implement leaves; a 64-section one arrives and takes all 64 bits from PGN 229.
        imp.stop()
        imp = None
        t = run(15.0, lambda: status()["taskController"]["sections"] == 0)
        check(t is not None, "implement gone is noticed (%s)" % ("%.1f s" % t if t else "never"))
        imp = Implement(implement_bin, 64)
        t = run(30.0, lambda: imp.connected and status()["taskController"]["sections"] == 64)
        check(t is not None, "a 64-section implement connects (%s s)" % ("%.1f" % t if t else "never"))
        pattern = 0x8000F00F0000A5A5
        app.sections = pattern
        app.send64 = True
        t = run(3.0, lambda: imp.sections == bits(pattern, 64))
        check(t is not None, "PGN 229: all 64 sections reach the implement (%s)" % ("%.0f ms" % (t * 1000) if t else imp.sections))
        run(6.0, lambda: status()["taskController"]["actual"] == bits(pattern, 64))
        check(status()["taskController"]["actual"] == bits(pattern, 64), "status shows all 64 as the implement reports them")
        t = run(1.0, lambda: app.status240 == (0x80, 1, 64, pattern, 10))
        check(t is not None, "PGN 240 carries all 64")

        # 6. The daemon stops: sections off on the way out.
        proc.terminate()
        t = run(3.0, lambda: imp.sections == bits(0, 64))
        check(t is not None, "daemon stopping: all sections off first")
    finally:
        if imp:
            imp.stop()
        proc.terminate()
        try:
            code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = None
        check(code == 0, "the daemon stops cleanly (exit %s)" % code)

    print("PASS" if ok else "FAILED (daemon log: %s)" % os.path.join(cfgdir, "daemon.log"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
