#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

"""AOW HAT key monitor. Separate from the daemon so an app or daemon crash cannot stop a
shutdown (design doc section 7). Reads KEY_SENSE (ADC128S102 IN4 on /dev/spidev0.3):
0.0758 x V_key, so on above about 8 V (0.61 V), off below about 4 V (0.30 V). Key off for
3 s -> systemctl poweroff. Booting with the key already off (after a watchdog reset, say)
shuts down again, after the same 3 s. The sense wire (IN2) is logged once at start, and a
warning goes to the journal if both collapse together (both wires on switched 12 V).

Sharing /dev/spidev0.3 with the daemon is fine: each ioctl is one complete SPI message, and the
channel to read is selected inside that same message.
"""
import array, fcntl, os, struct, subprocess, sys, time

DEV = "/dev/spidev0.3"
VREF = float(os.environ.get("AOW_ADC_REF", "5.0"))
PERIOD = 0.25
OFF_SECONDS = 3.0
ON_V, OFF_V = 0.61, 0.30  # at the ADC pin
SPI_IOC_WR_MODE = 0x40016b01
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046b04
SPI_IOC_MESSAGE_1 = 0x40206b00

def log(msg):
    print(msg, flush=True)

def read_channels(fd):
    tx = bytearray()
    for ch in list(range(8)) + [0]:
        tx += bytes([(ch & 7) << 3, 0])
    rx = array.array('B', bytes(len(tx)))
    txa = array.array('B', bytes(tx))
    msg = struct.pack('QQIIHBBBBBB', txa.buffer_info()[0], rx.buffer_info()[0], len(tx), 1000000, 0, 8, 0, 0, 0, 0, 0)
    fcntl.ioctl(fd, SPI_IOC_MESSAGE_1, msg)
    r = bytes(rx)
    return [(((r[2 * (c + 1)] << 8) | r[2 * (c + 1) + 1]) & 0xFFF) * VREF / 4096 for c in range(8)]

def main():
    fd = open(DEV, 'rb+', buffering=0)
    fcntl.ioctl(fd, SPI_IOC_WR_MODE, struct.pack('B', 3))
    fcntl.ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack('I', 1000000))
    v = read_channels(fd)
    log("keymon: key %.1f V, sense %.1f V at start (reference %.2f V)" % (v[4] / 0.0758, v[2] / 0.0758, VREF))
    key_on = v[4] >= ON_V
    off_since = None if key_on else time.monotonic()
    warned = False
    while True:
        time.sleep(PERIOD)
        try:
            v = read_channels(fd)
        except OSError as e:
            log("keymon: ADC read failed: %s" % e)
            continue
        if key_on and v[4] < OFF_V:
            key_on = False
            off_since = time.monotonic()
            log("keymon: key off (%.1f V); shutdown in %.0f s unless it returns" % (v[4] / 0.0758, OFF_SECONDS))
            if v[2] < OFF_V and not warned:
                warned = True
                log("keymon: WARNING the 12 V sense wire (J3.1) fell with the key: both wires are on switched 12 V")
        elif not key_on and v[4] >= ON_V:
            key_on = True
            off_since = None
            log("keymon: key on (%.1f V)" % (v[4] / 0.0758))
        if off_since is not None and time.monotonic() - off_since >= OFF_SECONDS:
            log("keymon: key off for %.0f s: poweroff" % OFF_SECONDS)
            subprocess.call(["systemctl", "poweroff"])
            off_since = None  # don't call it again while the system goes down

if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError:
        log("keymon: %s missing: no HAT overlay?" % DEV)
        sys.exit(1)
