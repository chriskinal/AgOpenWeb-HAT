#!/usr/bin/env python3
"""The status screen on a tractor's terminal, on virtual CAN: aow-hatd on CAN2 (vcan1), a
stand-in virtual terminal (build/isobus_vt) and a simulated implement beside it, and this script
as the app.

    sudo modprobe vcan; for i in 0 1 2; do sudo ip link add vcan$i type vcan; sudo ip link set vcan$i up; done
    python3 tests/isobus_vt_probe.py build/aow-hatd build/isobus_vt build/isobus_implement

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

# Object IDs in third_party/aog-taskcontroller/AOG_TC.iop.h
MASK_STATUS, MASK_IMPLEMENT = 1000, 1003
NAV_IMPLEMENT = 5004
CONFIG_TECU = 10004
STATUS, IMPLEMENT_SUMMARY, IP = 11000, 11004, 11005
SECTION_MAP, LINK, ALARM, TECU_LABEL = 11205, 11302, 11304, 11504
SPEED = 12000


def pgn(num, data, src=0x7F):
    body = bytes([src, num, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


class Lines:
    """A child process whose stdout is read a line at a time without blocking."""
    def __init__(self, args):
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.buf = b""

    def lines(self):
        if select.select([self.proc.stdout], [], [], 0)[0]:
            self.buf += self.proc.stdout.read() or b""
        out = []
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            out.append(line.decode(errors="replace"))
        return out

    def send(self, text):
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)


class Terminal(Lines):
    def __init__(self, binary, identity=3):
        super().__init__([binary, "vcan1", str(identity)])
        self.connected = False
        self.pool = None
        self.mask = None
        self.strings = {}
        self.numbers = {}

    def poll(self):
        for line in self.lines():
            key, _, rest = line.partition(" ")
            if key == "connected":
                self.connected = True
            elif key == "pool":
                self.pool = rest
            elif key == "mask":
                self.mask = int(rest)
            elif key in ("string", "number"):
                ident, _, value = rest.partition(" ")
                if key == "string":
                    self.strings[int(ident)] = value
                else:
                    self.numbers[int(ident)] = int(value)

    def text(self, ident):
        return self.strings.get(ident, "").strip()


def status(port=WEB_PORT):
    with urllib.request.urlopen("http://127.0.0.1:%d/api/status" % port, timeout=2) as r:
        return json.load(r)["isobus"]


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    vt_bin = sys.argv[2] if len(sys.argv) > 2 else "build/isobus_vt"
    implement_bin = sys.argv[3] if len(sys.argv) > 3 else "build/isobus_implement"
    cfgdir = tempfile.mkdtemp()
    settings = os.path.join(cfgdir, "settings.json")
    ok = True

    def check(cond, what):
        nonlocal ok
        ok = ok and bool(cond)
        print(("ok    " if cond else "FAIL  ") + what)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", APP_PORT))
    sock.setblocking(False)
    state = {"sections": 0, "silent": False}
    vt = None
    imp = None

    def run(seconds, until=None):
        """Plays the app at 50 Hz. Returns the time until the condition held, or None."""
        start = time.time()
        while time.time() - start < seconds:
            try:
                while True:
                    sock.recv(2048)
            except BlockingIOError:
                pass
            if not state["silent"]:
                s = state["sections"]
                sock.sendto(pgn(254, struct.pack("<HBhbBB", 50, 0, 0, 12, s & 0xFF, (s >> 8) & 0xFF)), ("127.0.0.1", DAEMON_PORT))
            if vt:
                vt.poll()
            if imp:
                imp.lines()
            if until and until():
                return time.time() - start
            time.sleep(0.02)
        return None if until else seconds

    def start(isobus):
        with open(settings, "w") as f:
            json.dump({"isobus": isobus}, f)
        return subprocess.Popen([daemon, "--config", settings, "--web-port", str(WEB_PORT), "--can", "vcan0,vcan1,vcan2",
                                 "--gvret-port", "2323", "--listen-port", str(DAEMON_PORT), "--app-port", str(APP_PORT)],
                                stderr=open(os.path.join(cfgdir, "daemon.log"), "a"))

    def stop(proc):
        proc.terminate()
        try:
            return proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            return None

    proc = start({"enabled": True, "channel": 2})
    try:
        # 1. A terminal appears: the daemon uploads the screen and the terminal accepts it.
        run(1.5)
        check(status()["terminal"] == "waiting", "no terminal on the bus: status says waiting")
        vt = Terminal(vt_bin)
        t = run(30.0, lambda: vt.connected and status()["terminal"] == "connected")
        check(t is not None, "the terminal takes the screen (%s s), pool %s" % ("%.1f" % t if t else "never", vt.pool))
        check(vt.pool == "ok", "the terminal's parser accepts the object pool")
        t = run(5.0, lambda: vt.mask == MASK_STATUS and vt.text(STATUS) == "OK")
        check(t is not None, "status page is up and says OK (mask %s, '%s')" % (vt.mask, vt.text(STATUS)))
        t = run(3.0, lambda: vt.text(LINK) == "ONLINE" and vt.text(ALARM) == "Waiting for implement")
        check(t is not None, "app online, waiting for an implement ('%s', '%s')" % (vt.text(LINK), vt.text(ALARM)))
        t = run(3.0, lambda: 1380 <= vt.numbers.get(SPEED, 0) <= 1395)
        check(t is not None, "speed 5.0 km/h shows as mm/s (%s)" % vt.numbers.get(SPEED))
        check(vt.text(TECU_LABEL) == "TECU: ON (restart req)", "settings page shows the tractor ECU role on")

        # 2. An implement: its name, section count and reported sections.
        imp = Lines([implement_bin, "vcan1", "16"])
        t = run(30.0, lambda: "Sections         16" in vt.strings.get(IMPLEMENT_SUMMARY, "") and vt.text(ALARM) == "No active alarms")
        check(t is not None, "the implement shows: %s" % vt.strings.get(IMPLEMENT_SUMMARY, "").replace("|", " / "))
        state["sections"] = 0x0005
        t = run(5.0, lambda: vt.text(SECTION_MAP).startswith("1 0 1 0 0"))
        check(t is not None, "sections 1 and 3 on show in the section map (%s)" % vt.text(SECTION_MAP)[:31])

        # 3. The operator's side: a soft key changes the page; the settings switch is saved.
        vt.send("key %d" % NAV_IMPLEMENT)
        t = run(3.0, lambda: vt.mask == MASK_IMPLEMENT)
        check(t is not None, "the IMPLEMENT soft key shows the implement page (mask %s)" % vt.mask)
        vt.send("num %d 0" % CONFIG_TECU)

        def saved_off():
            try:
                return json.load(open(settings))["isobus"]["tractorEcu"] is False
            except (ValueError, KeyError):
                return False
        t = run(5.0, lambda: saved_off() and vt.text(TECU_LABEL) == "TECU: OFF (restart req)")
        check(t is not None, "the tractor ECU switch on the terminal is saved to the settings ('%s')" % vt.text(TECU_LABEL))

        # 4. The app goes away.
        state["silent"] = True
        t = run(5.0, lambda: vt.text(LINK) == "OFFLINE" and vt.text(STATUS) == "OFF")
        check(t is not None, "app gone: OFFLINE on the diagnostics page ('%s', '%s')" % (vt.text(LINK), vt.text(STATUS)))
        state["silent"] = False

        # 5. That terminal leaves and a different one (another NAME) arrives.
        vt.stop()
        vt = None
        t = run(10.0, lambda: status()["terminal"] == "waiting")
        check(t is not None, "terminal gone is noticed (%s)" % ("%.1f s" % t if t else "never"))
        vt = Terminal(vt_bin, identity=7)
        t = run(30.0, lambda: vt.connected and vt.text(STATUS) == "OK" and status()["terminal"] == "connected")
        check(t is not None, "a different terminal takes the screen (%s)" % ("%.1f s" % t if t else "never"))
    finally:
        code = stop(proc)
        check(code == 0, "the daemon stops cleanly (exit %s)" % code)
        if vt:
            vt.stop()
            vt = None
        if imp:
            imp.stop()
            imp = None

    # 6. Switched off in the settings: a terminal is left alone.
    proc = start({"enabled": True, "channel": 2, "terminal": False})
    try:
        run(1.5)
        vt = Terminal(vt_bin)
        run(6.0)
        check(not vt.connected and status()["terminal"] == "off", "terminal off in the settings: no screen is offered")
    finally:
        code = stop(proc)
        check(code == 0, "the daemon stops cleanly (exit %s)" % code)
        if vt:
            vt.stop()

    print("PASS" if ok else "FAILED (daemon log: %s)" % os.path.join(cfgdir, "daemon.log"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
