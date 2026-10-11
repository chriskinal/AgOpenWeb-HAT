#!/usr/bin/env python3
"""ISOBUS address claims and the tractor ECU role on virtual CAN: starts aow-hatd with ISOBUS on
CAN2 (vcan1) and plays the other control functions on the bus, and the app for its speed.

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
TECU_ADDRESS = 240        # the tractor ECU's fixed address
TECU_FUNCTION = 134       # NAME function code: tractor ECU
WEB_PORT = 8089
DAEMON_PORT = 18888       # own UDP ports, so AgOpenWeb can stay up
APP_PORT = 19999


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


def frames(bus, seconds):
    """Every extended frame seen within the time: a list of (id, data)."""
    out = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            frame = bus.recv(16)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        can_id, dlc, data = struct.unpack("=IB3x8s", frame)
        if can_id & CAN_EFF:
            out.append((can_id & 0x1FFFFFFF, data[:dlc]))
    return out


def pgn254(speed_kmh):
    body = bytes([0x7F, 254, 8]) + struct.pack("<HBhbBB", int(speed_kmh * 10), 0, 0, 0, 0, 0)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def start(daemon, settings_json):
    cfgdir = tempfile.mkdtemp()
    settings = os.path.join(cfgdir, "settings.json")
    with open(settings, "w") as f:
        json.dump(settings_json, f)
    return subprocess.Popen([daemon, "--config", settings, "--web-port", str(WEB_PORT), "--can", "vcan0,vcan1,vcan2",
                             "--gvret-port", "2323", "--listen-port", str(DAEMON_PORT), "--app-port", str(APP_PORT)],
                            stderr=subprocess.DEVNULL)


def stop(proc):
    proc.terminate()
    try:
        return proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        return None


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    bus = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    bus.bind(("vcan1",))
    bus.setblocking(False)
    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.bind(("127.0.0.1", APP_PORT))
    ok = True

    def check(cond, what):
        nonlocal ok
        ok = ok and bool(cond)
        print(("ok    " if cond else "FAIL  ") + what)

    proc = start(daemon, {"isobus": {"enabled": True, "channel": 2}})
    try:
        # 1. The claim: preferred address, and a NAME that says task controller.
        seen = [c for c in claims(bus, 1.5) if (c[1] >> 40) & 0xFF == TC_FUNCTION]
        check(seen, "an address claim is sent")
        addr, name = seen[-1] if seen else (0xFE, 0)
        check(addr == TC_ADDRESS, "claimed the task controller's address 247 (got %d)" % addr)
        check(name >> 63 == 1 and (name >> 60) & 7 == 2, "NAME: arbitrary address capable, industry group 2")
        st = status()
        check(st["state"] == "claimed" and st.get("address") == TC_ADDRESS and st["channel"] == 2
              and st["interface"] == "vcan1" and st["busActive"], "/api/status shows the claim: %s" % st)
        check(int(st["name"], 16) == name, "/api/status shows the NAME on the bus")
        check(st["tractorEcu"]["state"] == "waiting", "the tractor ECU role waits: is there one on the bus?")

        # 2. A request for address claimed (PGN 59904 asking for 60928) is answered.
        claims(bus, 0.2)
        bus.send(can_frame(0x18EAFFFE, [0x00, 0xEE, 0x00]))
        again = claims(bus, 0.6)
        check((TC_ADDRESS, name) in again, "answers a request for address claimed")

        # 3. No tractor ECU on the bus after 2 s: we stand in at 240 and send speed.
        tecu = [c for c in claims(bus, 3.0) if (c[1] >> 40) & 0xFF == TECU_FUNCTION]
        check(tecu and tecu[-1][0] == TECU_ADDRESS and tecu[-1][1] >> 63 == 0,
              "stands in as the tractor ECU: claims 240, not arbitrary address capable")
        deadline = time.time() + 2.0
        seen = []
        while time.time() < deadline:
            app.sendto(pgn254(5.0), ("127.0.0.1", DAEMON_PORT))
            seen += frames(bus, 0.05)
        ground = [struct.unpack("<H", d[:2])[0] for i, d in seen if (i >> 8) & 0xFFFF == 0xFE49 and i & 0xFF == TECU_ADDRESS]
        wheel = [d for i, d in seen if (i >> 8) & 0xFFFF == 0xFE48 and i & 0xFF == TECU_ADDRESS]
        j1939 = [struct.unpack("<H", d[2:4])[0] for i, d in seen if (i >> 8) & 0xFFFF == 0xFEE8 and i & 0xFF == TECU_ADDRESS]
        check(ground and abs(ground[-1] - 1389) <= 2, "ground speed on the bus is the app's 5.0 km/h (%s mm/s)" % (ground[-1] if ground else None))
        check(wheel and j1939 and abs(j1939[-1] - 5 * 256) <= 2, "wheel speed and J1939 speed are sent too")
        st = status()["tractorEcu"]
        check(st["state"] == "active" and abs(st["speed"] - 5.0) < 0.1, "/api/status shows it: %s" % st)

        # 4. A real tractor ECU turns up: ours stops, whichever NAME would win the address.
        bus.send(can_frame(0x18EEFF00 | TECU_ADDRESS, struct.pack("<Q", (2 << 60) | (TECU_FUNCTION << 40) | 77)))
        frames(bus, 1.0)
        after = [i for i, d in frames(bus, 1.0) if (i >> 8) & 0xFFFF in (0xFE48, 0xFE49, 0xFEE8)]
        check(not after, "a tractor ECU appears: our speed messages stop")
        check(status()["tractorEcu"]["state"] == "tractor has one", "/api/status shows the tractor has one")

        # 5. A control function with a higher-priority NAME takes 247: we move.
        claims(bus, 0.1)
        rival = 0x0000000000000001
        bus.send(can_frame(0x18EEFF00 | TC_ADDRESS, struct.pack("<Q", rival)))
        moved = [c for c in claims(bus, 2.0) if c[1] == name]
        check(moved and moved[-1][0] != TC_ADDRESS and 128 <= moved[-1][0] <= 247,
              "gives up 247 to a higher-priority NAME and claims another (%s)" % (moved[-1][0] if moved else None))
        st = status()
        check(moved and st["state"] == "claimed" and st.get("address") == moved[-1][0],
              "/api/status follows the new address: %s" % st)
    finally:
        check(stop(proc) == 0, "the daemon stops cleanly")

    # 6. A tractor ECU that is there from the start: we never claim 240.
    time.sleep(0.5)
    frames(bus, 0.1)
    proc = start(daemon, {"isobus": {"enabled": True, "channel": 2}})
    try:
        ours = []
        end = time.time() + 5.0
        while time.time() < end:  # the tractor's ECU, answering as a real one would
            bus.send(can_frame(0x18EEFF00 | TECU_ADDRESS, struct.pack("<Q", (2 << 60) | (TECU_FUNCTION << 40) | 77)))
            ours += [c for c in claims(bus, 0.3) if c[0] == TECU_ADDRESS and c[1] & 0x1FFFFF != 77]
        check(not ours, "with a tractor ECU on the bus from the start, 240 is never claimed")
        check(status()["tractorEcu"]["state"] == "tractor has one", "/api/status shows the tractor has one")
    finally:
        check(stop(proc) == 0, "the daemon stops cleanly")

    # 7. Switched off in the settings.
    proc = start(daemon, {"isobus": {"enabled": True, "channel": 2, "tractorEcu": False}})
    try:
        ours = [c for c in claims(bus, 4.0) if c[0] == TECU_ADDRESS]
        check(not ours and status()["tractorEcu"]["state"] == "off", "tractorEcu off in the settings: 240 is never claimed")
    finally:
        check(stop(proc) == 0, "the daemon stops cleanly")

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
