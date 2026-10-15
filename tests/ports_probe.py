#!/usr/bin/env python3
"""End-to-end test of the serial port settings with no hardware: pseudo-terminals stand in for
the GNSS receiver and the two RS-232 ports.

Writes a settings file that puts NMEA out (GGA only, 5 Hz) on RS-232 #1 and RTCM out on #2,
starts aow-hatd with --gnss and --rs232 on the ptys, then:
  - streams GGA and VTG at 10 Hz into the receiver and checks #1 gets every other GGA, no VTG;
  - sends RTCM to the corrections port and checks #2 gets it byte for byte, as the receiver does;
  - checks /api/status names the receiver's port, and shows lines coming in on #1.
Own ports (8889, 2234, web 8090), so it can run beside the service.

    python3 tests/ports_probe.py build/aow-hatd
"""
import functools
import json
import os
import pty
import select
import socket
import subprocess
import sys
import time
import tty
import urllib.request


def nmea(body):
    cs = functools.reduce(lambda a, c: a ^ ord(c), body, 0)
    return ("$%s*%02X\r\n" % (body, cs)).encode()


def new_pty():
    m, s = pty.openpty()
    tty.setraw(m)
    tty.setraw(s)
    return m, s


def read_all(fd, wait=0.2):
    out = b""
    end = time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0.0, end - time.time()))
        if r:
            out += os.read(fd, 4096)
    return out


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build/aow-hatd"
    gps_m, gps_s = new_pty()
    p1_m, p1_s = new_pty()
    p2_m, p2_s = new_pty()
    cfg = "/tmp/ports_probe_settings.json"
    with open(cfg, "w") as f:
        json.dump({"ports": {"gnss": "onboard", "radio": "none", "keya": "none", "nmeaOut": "port1",
                             "rtcmOut": "port2", "baud": [38400, 115200], "nmeaSentences": 1, "nmeaRateHz": 5}}, f)

    proc = subprocess.Popen([daemon, "--gnss", os.ttyname(gps_s), "--gnss-baud", "115200",
                             "--rs232", "%s,%s" % (os.ttyname(p1_s), os.ttyname(p2_s)),
                             "--listen-port", "8889", "--rtcm-port", "2234", "--web-port", "8090",
                             "--gvret-port", "0", "--config", cfg])
    time.sleep(0.5)
    ok = True

    # 2 s of GGA and VTG at 10 Hz: NMEA out gets GGA only, about 5 a second.
    got = b""
    for i in range(20):
        t = time.gmtime()
        stamp = time.strftime("%H%M%S", t) + ".%d0" % (i % 10)
        os.write(gps_m, nmea("GNVTG,054.7,T,034.4,M,005.5,N,010.2,K,A"))
        os.write(gps_m, nmea("GNGGA,%s,4807.0380,N,01131.0000,E,4,24,0.6,545.4,M,46.9,M,1.2,0000" % stamp))
        got += read_all(p1_m, 0.1)
    got += read_all(p1_m, 0.2)
    lines = [l for l in got.decode(errors="replace").split("\r\n") if l]
    gga = [l for l in lines if l.startswith("$GNGGA")]
    print("NMEA out: %d sentences, %d GGA, first %s" % (len(lines), len(gga), lines[0] if lines else "-"))
    ok &= len(gga) == len(lines) and 8 <= len(gga) <= 12

    # RTCM from the app's port: to the receiver and to RTCM out, the same bytes.
    rtcm = bytes([0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x02, 0x02, 0x55, 0xAA])
    read_all(gps_m, 0.05)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(rtcm, ("127.0.0.1", 2234))
    out = read_all(p2_m, 0.3)
    to_rx = read_all(gps_m, 0.1)
    print("RTCM out: %d bytes, receiver %d bytes" % (len(out), len(to_rx)))
    ok &= out == rtcm and to_rx == rtcm

    # Something talking back on the NMEA out port: the page sees the first 20 characters of each line.
    os.write(p1_m, b"HELLO FROM A RATE CONTROLLER 123\r\nSECOND LINE\r\n")
    time.sleep(0.3)
    st = json.load(urllib.request.urlopen("http://127.0.0.1:8090/api/status", timeout=2))
    p1 = st.get("serial", {}).get("rs232", [{}])[0]
    print("port 1 in: %s" % p1)
    ok &= p1.get("use") == "nmeaOut" and p1.get("lines") == ["HELLO FROM A RATE CO", "SECOND LINE"]
    print("status serial: %s" % st.get("serial"))
    ok &= st.get("serial", {}).get("gnssPort") == os.ttyname(gps_s) and st["serial"]["nmeaOutSent"] >= 8
    ok &= st["serial"]["rtcmOutBytes"] == len(rtcm)

    proc.terminate()
    proc.wait(timeout=5)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
