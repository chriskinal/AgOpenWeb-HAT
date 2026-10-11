#!/usr/bin/env python3
"""ISOBUS address claim on virtual CAN: starts aow-hatd with ISOBUS on CAN2 (vcan1) and plays
the other control functions on the bus.

    sudo modprobe vcan; for i in 0 1 2; do sudo ip link add vcan$i type vcan; sudo ip link set vcan$i up; done
    python3 tests/isobus_probe.py build/aow-hatd

Stop the aow-hatd service first: this starts its own daemon on the same ports.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

CAN_EFF = 0x80000000
TC_ADDRESS = 247          # ISO 11783: the task controller's preferred address
TC_FUNCTION = 130         # NAME function code: task controller
WEB_PORT = 8089


def can_frame(can_id, data):
    return struct.pack("=IB3x8s", can_id | CAN_EFF, len(data), bytes(data).ljust(8, b"\0"))


def claims(bus, seconds):
    """Address claims (PGN 60928) seen within the time: a list of (source address, NAME)."""
    out = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            frame = bus.recv(16)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        can_id, dlc, data = struct.unpack("=IB3x8s", frame)
        if can_id & CAN_EFF and (can_id >> 16) & 0xFF == 0xEE and dlc == 8:
            out.append((can_id & 0xFF, struct.unpack("<Q", data)[0]))
    return out


def status():
    with urllib.request.urlopen("http://127.0.0.1:%d/api/status" % WEB_PORT, timeout=2) as r:
        return json.load(r)["isobus"]


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    cfgdir = tempfile.mkdtemp()
    settings = os.path.join(cfgdir, "settings.json")
    with open(settings, "w") as f:
        json.dump({"isobus": {"enabled": True, "channel": 2}}, f)

    bus = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    bus.bind(("vcan1",))
    bus.setblocking(False)

    proc = subprocess.Popen([daemon, "--config", settings, "--web-port", str(WEB_PORT), "--can",
                             "vcan0,vcan1,vcan2", "--gvret-port", "2323"], stderr=subprocess.DEVNULL)
    ok = True

    def check(cond, what):
        nonlocal ok
        ok = ok and bool(cond)
        print(("ok    " if cond else "FAIL  ") + what)

    try:
        # 1. The claim: preferred address, and a NAME that says task controller.
        seen = claims(bus, 3.0)
        check(seen, "an address claim is sent")
        addr, name = seen[-1] if seen else (0xFE, 0)
        check(addr == TC_ADDRESS, "claimed the task controller's address 247 (got %d)" % addr)
        check((name >> 40) & 0xFF == TC_FUNCTION, "NAME function is task controller")
        check(name >> 63 == 1 and (name >> 60) & 7 == 2, "NAME: arbitrary address capable, industry group 2")
        st = status()
        check(st["state"] == "claimed" and st.get("address") == TC_ADDRESS and st["channel"] == 2
              and st["interface"] == "vcan1" and st["busActive"], "/api/status shows the claim: %s" % st)
        check(int(st["name"], 16) == name, "/api/status shows the NAME on the bus")

        # 2. A request for address claimed (PGN 59904 asking for 60928) is answered.
        claims(bus, 0.2)
        bus.send(can_frame(0x18EAFFFE, [0x00, 0xEE, 0x00]))
        again = claims(bus, 1.0)
        check((TC_ADDRESS, name) in again, "answers a request for address claimed")

        # 3. A control function with a higher-priority NAME takes 247: we move.
        rival = 0x0000000000000001
        bus.send(can_frame(0x18EEFF00 | TC_ADDRESS, struct.pack("<Q", rival)))
        moved = [c for c in claims(bus, 2.0) if c[1] == name]
        check(moved and moved[-1][0] != TC_ADDRESS and 128 <= moved[-1][0] <= 247,
              "gives up 247 to a higher-priority NAME and claims another (%s)" % (moved[-1][0] if moved else None))
        st = status()
        check(moved and st["state"] == "claimed" and st.get("address") == moved[-1][0],
              "/api/status follows the new address: %s" % st)
    finally:
        proc.terminate()
        try:
            code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = None
        check(code == 0, "the daemon stops cleanly (exit %s)" % code)

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
