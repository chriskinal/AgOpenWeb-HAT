#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""Control-loop timing under CAN and CPU load, on the dev CM4 (CAN2 and CAN3 linked).

Phases of SECS seconds each: baseline; CAN3 flooding CAN2 at bus rate; the flood plus one
normal-priority busy loop per core (a stand-in for a busy aow-vtd). Reads aow-hatd's
/api/status "loop" after every 10 s window.
"""
import json, multiprocessing, os, socket, struct, sys, time, urllib.request

SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 60
TX, RX = "hatcan3", "hatcan2"


def status():
    with urllib.request.urlopen("http://127.0.0.1/api/status", timeout=2) as r:
        return json.load(r)["loop"]


def rx_packets(dev):
    with open(f"/sys/class/net/{dev}/statistics/rx_packets") as f:
        return int(f.read())


def flood(stop):
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((TX,))
    # Priority 7, proprietary B 0xFF80, from 0x80: loses arbitration to every ISOBUS message.
    frame = struct.pack("=IB3x8s", 0x1CFF8080 | socket.CAN_EFF_FLAG, 8, bytes(range(8)))
    while not stop.is_set():
        try:
            s.send(frame)
        except OSError:
            time.sleep(0.001)  # transmit queue full


def burn(stop):
    while not stop.is_set():
        pass


def phase(name, can, burners):
    stop = multiprocessing.Event()
    procs = []
    if can:
        procs.append(multiprocessing.Process(target=flood, args=(stop,)))
    procs += [multiprocessing.Process(target=burn, args=(stop,)) for _ in range(burners)]
    for p in procs:
        p.start()
    time.sleep(1)
    rx0, t0 = rx_packets(RX), time.time()
    worst = {"lateMaxUs": 0, "workMaxUs": 0, "inputsMaxUs": 0}
    l0 = status()
    for _ in range(SECS // 10):
        time.sleep(10)
        l = status()
        for k in worst:
            worst[k] = max(worst[k], l[k])
    l1 = status()
    rate = (rx_packets(RX) - rx0) / (time.time() - t0)
    stop.set()
    for p in procs:
        p.join()
    print(f"{name:28s} CAN2 rx {rate:6.0f}/s  late max {worst['lateMaxUs']:5d} us  "
          f"work max {worst['workMaxUs']:5d} us  inputs max {worst['inputsMaxUs']:5d} us  "
          f"missed +{l1['missed'] - l0['missed']}  >1ms late +{l1['lateOver1ms'] - l0['lateOver1ms']}",
          flush=True)
    time.sleep(2)


if __name__ == "__main__":
    print(f"{SECS} s per phase, {os.cpu_count()} cores", flush=True)
    phase("baseline", False, 0)
    phase("CAN flood", True, 0)
    phase("CAN flood + CPU on all cores", True, os.cpu_count())
    phase("baseline again", False, 0)
    print("since start:", json.dumps(status()))
