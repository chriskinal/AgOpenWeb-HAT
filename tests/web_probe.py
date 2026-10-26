#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""Checks a running aow-hatd's web side: every page and API, the telemetry WebSocket (8082)
and the log WebSocket (8083). Standard library only.

    python3 tests/web_probe.py --port 8080 --settings /tmp/aow/settings.json
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request


def http(base, path, body=None):
    req = urllib.request.Request(base + path, data=body.encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    with urllib.request.urlopen(req, timeout=3) as r:
        return r.status, r.headers.get("Content-Type", ""), r.read().decode()


class WebSocket:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.s.recv(1)
        if not head.startswith(b"HTTP/1.1 101"):
            raise RuntimeError("handshake refused: %r" % head[:40])
        self.buf = b""

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def recv(self):
        self._need(2)
        op, ln = self.buf[0] & 0x0F, self.buf[1] & 0x7F
        hdr = 2
        if ln == 126:
            self._need(4)
            ln, hdr = struct.unpack(">H", self.buf[2:4])[0], 4
        elif ln == 127:
            self._need(10)
            ln, hdr = struct.unpack(">Q", self.buf[2:10])[0], 10
        self._need(hdr + ln)
        data, self.buf = self.buf[hdr:hdr + ln], self.buf[hdr + ln:]
        return op, data

    def close(self):
        self.s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--settings", help="settings file the daemon writes, to check a save lands")
    a = ap.parse_args()
    base = "http://%s:%d" % (a.host, a.port)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    for path, marker in [("/", "AgOpenWeb HAT"), ("/device", "invertWorkSwitch"), ("/eventlogger", "serialLevel"),
                         ("/logs", ":8083"), ("/network", "/api/network/config"), ("/ota", ".aowpkg"), ("/isobus", "/api/isobus"), ("/serial", "/api/serial"), ("/touch.css", "{")]:
        code, ctype, body = http(base, path)
        check(code == 200 and marker in body, "GET %-14s %d %s, %d bytes" % (path, code, ctype.split(";")[0], len(body)))

    st = json.loads(http(base, "/api/status")[2])
    check("version" in st and st["moduleId"] == 126,
          "/api/status: version %s, uptime %d ms, ip %s" % (st["version"], st["uptime"], st["network"]["ip"]))
    wifi = json.loads(http(base, "/api/wifi")[2])
    check("mode" in wifi and "ap" in wifi, "/api/wifi: %s, %s" % (wifi["mode"], (wifi["ssid"] or "not connected") if wifi["available"] else wifi["unavailable"]))
    dev = json.loads(http(base, "/api/device/settings")[2])
    check("softStartDuration" in dev, "/api/device/settings: %s" % dev)
    ev = json.loads(http(base, "/api/eventlogger/config")[2])
    check("serialLevel" in ev, "/api/eventlogger/config: %s" % ev)
    logs = json.loads(http(base, "/api/logs/data")[2])["logs"]
    check(len(logs) > 0, "/api/logs/data: %d entries, newest: %s" % (len(logs), logs[-1]["message"] if logs else "-"))

    tel = WebSocket(a.host, 8082)
    t0, n, last = time.time(), 0, None
    while time.time() - t0 < 1.0:
        op, data = tel.recv()
        if op == 2:
            n, last = n + 1, data
    tel.close()
    check(80 <= n <= 110 and last is not None and len(last) == 32, "telemetry 8082: %d packets in 1 s, 32 bytes each" % n)

    lg = WebSocket(a.host, 8083)
    lg.s.settimeout(2)
    try:
        hist = json.loads(lg.recv()[1])
    except (socket.timeout, EOFError):
        hist = {}
    check(hist.get("type") == "history" and len(hist.get("logs", [])) > 0 and "message" in hist["logs"][-1],
          "log 8083: history on connect, %d entries" % len(hist.get("logs", [])))
    new_led = 50 if dev.get("ledBrightness") != 50 else 75
    code, _, body = http(base, "/api/device/settings", json.dumps({"ledBrightness": new_led}))
    check(json.loads(body).get("status") == "saved", "POST ledBrightness=%d -> %s" % (new_led, body))
    got = None
    lg.s.settimeout(2)
    try:
        while got is None:
            op, data = lg.recv()
            entry = json.loads(data)
            if "Settings saved" in entry["message"]:
                got = entry
    except (socket.timeout, EOFError):
        pass
    lg.close()
    check(got is not None, "log 8083: %s" % (got and "%s/%s %s" % (got["severityName"], got["sourceName"], got["message"])))
    check(json.loads(http(base, "/api/device/settings")[2])["ledBrightness"] == new_led, "setting reads back")
    if a.settings:
        with open(a.settings) as f:
            saved = json.load(f)
        check(saved["misc"]["ledBrightness"] == new_led, "%s has ledBrightness %d" % (a.settings, new_led))

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
