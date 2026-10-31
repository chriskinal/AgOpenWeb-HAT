<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The AgOpenWeb-HAT contributors -->

# AgOpenWeb HAT

The **AOW HAT** board design and its hardware daemon, `aow-hatd`.

The HAT is a 40-pin board for the Raspberry Pi CM4 IO Board. It replaces an AgOpenGPS AiO board,
and the daemon does the work the AiO board's Teensy firmware did: steering, wheel-angle sensing,
GNSS, RTK corrections and tractor CAN.

It talks to [AgOpenWeb](https://github.com/AgOpenGPS-Official/AgOpenWeb) in the AgOpenGPS PGN
protocol over UDP on the loopback interface, so to the app it looks like an AiO board.

**Status.** Board: schematic verified, PCB routing in progress (see `hardware/hat/`).
Daemon: answers the app as a steer and GPS module (hello, scan, PGN 253 for each 254); turns
receiver NMEA into PANDA/PAOGI and forwards RTCM (`--gnss /dev/ttyUSB0` for a USB receiver).
Serves the firmware's web pages on port 80. Nothing drives steering hardware yet.
The software plan is in [docs/PLAN.md](docs/PLAN.md).

## Two ways to run AgOpenWeb on a Pi

| | Hardware | Steering, GNSS, CAN handled by |
|---|---|---|
| 1 | CM4 + AOW HAT | this daemon |
| 2 | any Pi + an existing AiO board (v4.x / v5.x) on Ethernet | the board's own Teensy firmware |

The installer asks once which one applies. With no HAT the daemon stays disabled.

A HAT release ([Releases](https://github.com/chriskinal/AgOpenWeb-HAT/releases)) carries AgOpenWeb
too, so the unit's software is one matched set: the `.aowpkg` goes on the unit's System Update page,
which installs whatever changed (the HAT software, AgOpenWeb, or both) and puts the previous set back
if the new one does not start. For a new unit, unpack the `.tar.gz` and run `sudo ./install.sh --hat`.

## Layout

| Folder | Contents |
|---|---|
| `core/` | logic with no hardware or Linux dependency; builds and tests on any desktop |
| `hal/` | the interfaces core uses to reach hardware |
| `boards/hat/` | the HAT's pin map and (later) its Linux drivers |
| `web/` | the firmware's web pages and API |
| `daemon/` | the Linux executable |
| `third_party/` | ArduinoJson (MIT) |
| `tests/` | unit tests for core |
| `deploy/` | installer and systemd unit |
| `hardware/hat/` | the HAT board: design, netlist, BOM, open issues |
| `hardware/aio/` | the earlier AiO board design (parked), with its EasyEDA parts and exports |
| `hardware/tools/` | schematic and PCB audit scripts, EasyEDA net-colouring extension |

## Build and test

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

The daemon itself builds only on Linux. Core and the tests build anywhere.

On a CM4 with Raspberry Pi OS Lite (Trixie):

```bash
sudo apt install -y git cmake g++ python3
git clone https://github.com/chriskinal/AgOpenWeb-HAT.git && cd AgOpenWeb-HAT
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build
./build/aow-hatd -v &                  # answers on UDP 8888, replies to 127.0.0.1:9999
python3 tests/pgn_probe.py --rate 100 -n 500
python3 tests/gnss_probe.py build/aow-hatd   # GNSS end to end, a pty playing the receiver
```

## Licence

Three licences, one per file (its `SPDX-License-Identifier` line); see [LICENSE](LICENSE) and
[LICENSES/](LICENSES/):

- **MIT** for what was written here: most of the daemon, the tests, the deploy scripts, the docs
  and the hardware tools.
- **GPL-3.0-or-later** for what is ported or copied from the AgOpenGPS Teensy AiO firmware (the
  CAN files, the autosteer, GNSS and IMU code, the web pages and their API, and others), and for
  AOG-TaskController in `third_party/`. `aow-hatd` as built includes them, so the program as a
  whole is GPL-3.0-or-later.
- **CERN-OHL-P-2.0** for the hardware designs in `hardware/` (not `hardware/tools/`).
