#!/usr/bin/env python3
"""Tractor CAN end to end on virtual CAN: starts aow-hatd with Valtra/MF steering on CAN3 and
plays both the app (UDP) and the steering valve (vcan2).

    sudo modprobe vcan; for i in 0 1 2; do sudo ip link add vcan$i type vcan; sudo ip link set vcan$i up; done
    python3 tests/can_probe.py build/aow-hatd

Stop AgOpenWeb first: this binds the app's port (9999).
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

VALVE_RX = 0x0CAC1C13  # valve -> module: curve, ready
STEER_TX = 0x0CAD131C  # module -> valve: curve, intent 253/252
CAN_EFF = 0x80000000


def pgn(num, data, src=0x7F):
    body = bytes([src, num, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def steer_data(guidance, target=0.0, speed=5.0):
    return pgn(254, struct.pack("<HBhbBB", int(speed * 10), 1 if guidance else 0, int(target * 100), 0, 0, 0))


def can_frame(can_id, data):
    return struct.pack("=IB3x8s", can_id | CAN_EFF, len(data), bytes(data).ljust(8, b"\0"))


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    cfgdir = tempfile.mkdtemp()
    settings = os.path.join(cfgdir, "settings.json")
    with open(settings, "w") as f:  # brand 9 (Valtra/MF), CAN3 = steering (function bit 1)
        json.dump({"can": {"brand": 9, "bus": [{"function": 0}, {"function": 0}, {"function": 1}]}}, f)

    valve = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    valve.bind(("vcan2",))
    valve.setblocking(False)
    app = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    app.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    app.bind(("0.0.0.0", 9999))
    app.setblocking(False)

    proc = subprocess.Popen([daemon, "--config", settings, "--web-port", "0", "--can", "vcan0,vcan1,vcan2",
                             "--gvret-port", "2323"],
                            stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    commands, popups, angles = [], [], []

    def pump(seconds, guidance, ready, curve=250):
        t_end = time.time() + seconds
        while time.time() < t_end:
            valve.send(can_frame(VALVE_RX, [curve & 0xFF, (curve >> 8) & 0xFF, 1 if ready else 0, 0, 0, 0, 0, 0]))
            app.sendto(steer_data(guidance), ("127.0.0.1", 8888))
            time.sleep(0.01)
            try:
                while True:
                    cid, n, data = struct.unpack("=IB3x8s", valve.recv(16))
                    if cid & 0x1FFFFFFF == STEER_TX:
                        commands.append((time.time(), data[:n]))
            except BlockingIOError:
                pass
            try:
                while True:
                    b = app.recv(512)
                    if len(b) > 5 and b[:2] == b"\x80\x81" and b[3] == 221:
                        popups.append(b[7:-1].decode(errors="replace"))
                    if len(b) == 14 and b[3] == 253:
                        angles.append((struct.unpack("<h", b[5:7])[0] / 100.0, b[11]))
            except BlockingIOError:
                pass

    pump(0.3, guidance=False, ready=True)  # settle, and drop what was sent before we listened
    commands.clear()
    pump(1.0, guidance=False, ready=True)
    idle = [c for c in commands]
    rate = len(idle)
    check(40 <= rate <= 60, "valve commands at ~50 Hz before engaging: %d in 1 s" % rate)
    check(idle and all(c[1][2] == 252 for c in idle), "all with intent 252 (no steer)")
    check(angles and abs(angles[-1][0] - 2.5) < 0.01, "PGN 253 angle from the CAN curve: %.2f deg" % (angles[-1][0] if angles else -1))

    commands.clear()
    pump(1.0, guidance=True, ready=True)
    steering = [c for c in commands if c[1][2] == 253]
    check(len(steering) > 30, "engaged: %d commands with intent 253" % len(steering))
    if steering:
        curve = struct.unpack("<h", steering[-1][1][0:2])[0]
        check(curve % 128 == 0 and curve != 0, "steer command curve %d (legacy Valtra: PWM x 128)" % curve)

    commands.clear()
    pump(0.6, guidance=True, ready=False)
    check(any("not ready" in p for p in popups), "popup on valve not ready: %s" % (popups[-1] if popups else "none"))
    late = [c for c in commands if c[0] > commands[0][0] + 0.2] if commands else []
    check(late and all(c[1][2] == 252 for c in late), "valve not ready: back to intent 252")

    # GVRET: SavvyCAN's handshake, then the mirrored steer commands on CAN3 (bus index 2).
    g = socket.create_connection(("127.0.0.1", 2323), timeout=2)
    g.sendall(b"\xE7\xF1\x07\xF1\x0C")
    time.sleep(0.2)
    pump(0.5, guidance=False, ready=True)
    g.settimeout(0.5)
    stream = b""
    try:
        while len(stream) < 4000:
            chunk = g.recv(65536)
            if not chunk:
                break
            stream += chunk
    except socket.timeout:
        pass
    g.close()
    check(stream.startswith(b"\xF1\x07\x84\x17") and b"\xF1\x0C\x03" in stream, "GVRET: device info and 3 buses")
    frames = [i for i in range(len(stream) - 11) if stream[i:i + 2] == b"\xF1\x00"
              and struct.unpack("<I", stream[i + 6:i + 10])[0] == (STEER_TX | CAN_EFF) and stream[i + 10] >> 4 == 2]
    check(len(frames) > 10, "GVRET: %d steer commands mirrored from CAN3" % len(frames))

    proc.terminate()
    proc.wait(timeout=3)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
