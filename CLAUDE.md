# CLAUDE.md - AgOpenWeb HAT

This file provides guidance to Claude Code when working with this repository.

**Key documentation:**
- **[docs/PLAN.md](docs/PLAN.md)** - Software plan: decisions, firmware-to-daemon mapping, safety rules, build phases
- **[docs/PLAN_VT.md](docs/PLAN_VT.md)** - The virtual terminal (the implement's screens in AgOpenWeb): runs in the app on SharpIsoTerm, its ISOBUS terminal library; this repo's part is the CAN gateway, `aow-linkd` (built, its status shown on the ISOBUS page)
- **[hardware/hat/HARDWARE_HAT_DESIGN.md](hardware/hat/HARDWARE_HAT_DESIGN.md)** - What the board does and why: mechanical, power, circuits, pinouts
- **[hardware/hat/HARDWARE_HAT_NETLIST.md](hardware/hat/HARDWARE_HAT_NETLIST.md)** - Every part and net
- **[hardware/hat/HARDWARE_HAT_ISSUES.md](hardware/hat/HARDWARE_HAT_ISSUES.md)** - Open decisions (H#) and bench checks (B#)
- **[hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md](hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md)** - LSM6DSK320X IMU breakout for the vibration evaluation: design, netlist, BOM, open items (K#)

## Project Overview

Two things live here:

1. **The AOW HAT board design**: a 40-pin HAT for the official Raspberry Pi CM4 IO Board. It
   replaces an AgOpenGPS AiO board. Designed in **EasyEDA Standard** (not Pro).
2. **`aow-hatd`**: the Linux daemon that runs on the CM4 and does what the AiO board's Teensy
   firmware did: steering, wheel-angle sensing, GNSS, RTK corrections, tractor CAN.

The daemon talks to AgOpenWeb in the **AgOpenGPS PGN protocol over UDP on loopback**, so to the app
the HAT looks like an AiO board.

### Deployment patterns

| | Hardware | Steering, GNSS, CAN handled by |
|---|---|---|
| 1 | CM4 + AOW HAT | `aow-hatd` (this repo) |
| 2 | any Pi + an existing AiO board (v4.x / v5.x) on Ethernet | the board's own Teensy firmware, unchanged |

**Never assume the HAT is present.** The installer asks once (`deploy/install.sh --hat|--no-hat`).
The Teensy firmware is not being rewritten and nothing is ported back to it.

### Related repositories

| Repo | Local path | Role |
|---|---|---|
| AgOpenWeb | `/Users/chris/Code/AgOpenWeb` | the guidance app. **App only: no hardware files go there.** |
| Teensy AiO firmware | `/Users/chris/Code/Firmware_Teensy_AiO_26` | reference the daemon is ported from (v26.5.5) |

## Status

- **Board:** built. Assembled boards arrived 2026-10-09 and one is fitted to the dev CM4's IO
  board with a UM982EB on J7. The power latch and every peripheral passed on the bench
  (issues doc section 2; docs/PLAN.md phase 1 record). What is left needs a vehicle or a load.
  4 layers: Top, Inner1 GND plane, Inner2 signal with a GND pour, Bottom. Rules 0.2 mm track and
  clearance, 0.5 mm power. Routed with EasyEDA's JLCPCB router, GND skipped; GND vias placed by
  hand, including one at each J3 protection diode. J1.17 left unconnected (issues H10).
- **Daemon:** runs on Linux and answers the app as steer (126) and GPS (120): hello, scan, and a
  PGN 253 for every PGN 254 (`core/app_link`). GNSS done in core (`nmea`, `gnss`, `nav`,
  `gnss_link`): receiver sentences in, PANDA/PAOGI out, RTCM through; on the HAT the receiver
  is `/dev/ttyAMA5`. Event logger, JSON settings file and the firmware's web pages
  (Home, Device, Event logger, Logs, Network) on port 80 with WebSockets 8082/8083, served by a
  normal-priority web thread. Steering logic in `core/steer` (arming, kickouts, timeouts, soft
  start, P controller, PGN 250/251/252), closed-loop on `--board sim` and, with a pot as the
  WAS, on `--board hat`. HAT drivers in `boards/hat`: ADC over spidev, switches and DIR/EN
  through the GPIO character device, PWM0 through sysfs, the STWD100 watchdog (kicked from
  the control loop), SK6812 LEDs from the PCM block, the piezo. Not yet run with a motor driver.
  Tractor CAN (all 9 brands, JSON engine, Keya CAN), Keya serial, CAN page and upload, GVRET on
  TCP 23: done and tested on vcan. The firmware's CAN files live in `core/src/can` near-verbatim.
  UM98x (GPS) page, radio RTCM and GPS passthrough: done. External IMU (`core/imu`): a BNO085
  (RVC) on an RS-232 port or a TM171 on USB (`/dev/ttyACMx`), reopened after an unplug: done on
  ptys, and a real TM171 on the dev CM4 (signs checked on the bench). What is on the
  RS-232 ports and USB (a receiver, radio or IMU) is found by listening (`daemon/serial_hub`,
  `core/serial_detect`, auto rate); only the new NMEA out and RTCM out and the port rates are
  set, on the Serial Communications page (`/serial`). NMEA out carries the app's corrected
  position (PGN 100, `core/app_link` to `GnssLink::onCorrectedFix`) while the app sends it. `--radio` and `--imu` take a device as
  such (bench); USB is looked at only with `--board hat` or `--usb`. LED and buzzer logic
  (`core/indicators`; `/api/status` shows the LED colours), `aow-hatctl`: done.
  PGNs are accepted only from the `--app` host (loopback); `--any-app` lifts that.
- **ISOBUS (docs/PLAN.md section 8):** AgIsoStack++ is built into the daemon
  (`daemon/isobus.cpp`, fetched by CMake; `-DAOW_ISOBUS=OFF` leaves it out) and runs
  AOG-TaskController's section control task controller on the channel in the `isobus`
  settings, off by default: the app's section bits (PGN 254, or 229 for 64) go to the implement,
  and what the implement reports comes back in PGN 240.
  It stands in as the tractor ECU (speed on the bus) when the bus has none, has its own
  page, `/isobus`, and puts upstream's status screen on a virtual terminal if the bus has one
  (`daemon/isobus_vt.cpp`). Works against a simulated implement and a stand-in terminal, on vcan
  and over two linked HAT channels; no real implement or terminal yet.
  The stack's pin in `CMakeLists.txt` follows upstream HEAD: move it before each package (and
  run the ISOBUS probes); it is not tied to AOG-TaskController's pin. Upstream fixes not yet
  merged ride on the pin as patches in `third_party/agisostack-patches/` (now PR #793, the
  transmit queue's lock, and PR #808, a TP/ETP session's first frame sent once): drop each at the
  first pin move after it is merged.
  `third_party/aog-taskcontroller/` is upstream's file **unmodified**: change the stand-in
  headers in `daemon/isobus_compat/` or `daemon/isobus.cpp` instead. Include the stack's
  headers before any `aow/` header and `#undef` its `LOG_*` macros, as `daemon/isobus.cpp` does.
- **AOW link gateway (`aow-linkd`, docs/PLAN_VT.md section 2):** its own program and service,
  off by default (the ISOBUS page's switch, or `install.sh --hat --gateway`). Carries every frame of `hatcan2` to AgOpenWeb's
  virtual terminal over TCP 2323 (the protocol: SharpIsoTerm `docs/TRANSPORT.md` section 5).
  Its code lives in SharpIsoTerm (`gateway/linux`, MIT), fetched by CMake at a pinned commit that
  follows SharpIsoTerm's main; this repo has its unit, settings and status. Its name and whether tablets on the network may
  connect are set on the ISOBUS page: `aow-hatd` writes `/etc/aow-hat/linkd.conf` (read by
  `aow-linkd --config`; `linkd.env` options still win) and, while the gateway is on and listens on
  the network, an avahi service file announcing it as `_aowlink._tcp`
  (`/etc/avahi/services/aow-linkd.service`), so AgOpenWeb's card lists it by name. Saving them
  restarts the gateway. A second terminal is refused as "in use by" the first one's name. It reports to `aow-hatd` (`--status 127.0.0.1:8891`, JSON, `aow/helper_status.h`),
  which shows it (`/api/status` `linkGateway`, the ISOBUS page) and never acts on it. Switched on
  and off on the ISOBUS page (`/api/linkgateway`: `systemctl enable|disable --now`) or by the
  installer; nothing else restarts.
- **Wi-Fi (Network page, `/api/wifi`):** client of known networks or the cab's access point
  (`aow-ap`, 10.42.0.1), through nmcli on a thread of its own (`daemon/wifi.cpp`; arguments and
  checks in `core/wifi`, no shell). A failed switch goes back; in client mode the access point
  is the fallback 90 s after boot when no known network is found (`/etc/aow-hat/wifi.conf`).
  On the dev CM4 (2026-10-26) client to access point and back worked from the page (up in 6 s,
  back on the home network in 13 s); the fallback after boot is not tried yet. A switch cuts
  the SSH session.
- **CAN watch:** `daemon/can_watch.cpp` restarts a CAN interface that has frames queued and sends
  none with no bus error (seen once after a bus-off storm; `/api/status` `canRestarts`).
- **Key monitor:** `deploy/aow-keymon.py`, its own service: key off for 3 s runs `poweroff`.
- **Updates:** `deploy/package.sh` (on Linux arm64) builds a bundle and a signed `.aowpkg`; the
  System Update page (`/ota`) takes the `.aowpkg`, and `deploy/aow-hat-update`, a separate root
  service, verifies, installs and rolls back. `deploy/update-key.pem` is a development key.
  Per-unit daemon options live in `/etc/aow-hat/daemon.env`, not in the service unit.
- **Scope:** port every v26 firmware function the HAT hardware supports, web UI included
  (docs/PLAN.md section 2).
- **Dev CM4:** `chris@192.168.4.217`, passwordless sudo, Trixie Lite with the packaged
  PREEMPT_RT kernel, HAT fitted. Runs `aow-hatd --board hat`, `aow-keymon` and AgOpenWeb
  (headless, UI on port 5174) as services. The hardware watchdog is live while the daemon
  runs: stop the service, do not kill the process. Device names: CAN `hatcan1`-`hatcan3`,
  GNSS `/dev/ttyAMA5`, RS-232 `/dev/ttyAMA2` and `/dev/ttyAMA3`, ADC `/dev/spidev0.3`.
  Build on the Mac (`deploy/dev.sh`, below) and install the package through its update page; the
  CM4 needs no compiler (an SD-card unit will not have one). Its `~/AgOpenWeb-HAT` rsync copy is
  only for running the Python probes there.

## Layout

```
AgOpenWeb-HAT/
├── core/            # Logic with no hardware or Linux dependency; builds on any desktop
├── hal/             # Interfaces core uses to reach hardware (aow/hal.h)
├── boards/hat/      # HAT pin map (hat_pins.h), device tree overlay and Linux drivers
├── web/             # The firmware's web pages (web/pages, copied) and API on POSIX sockets
├── daemon/          # The Linux executable: control thread + web thread
├── third_party/     # ArduinoJson 7.4.2 (the firmware's JSON library, MIT); AOG-TaskController's
│                    # task controller (GPL-3, unmodified)
├── tests/           # Unit tests for core
├── deploy/          # install.sh, service units, config.txt block, udev rule, key monitor
├── docs/PLAN.md
└── hardware/
    ├── hat/         # HAT design, netlist, BOM, issues
    ├── aio/         # Earlier AiO board design (parked) + EasyEDA parts and exports
    ├── imu-breakout/ # LSM6DSK320X evaluation breakout (one doc: design, netlist, BOM, K# items)
    └── tools/       # hat-audit scripts, EasyEDA net-colouring extension
```

## Build Commands

```bash
# Build and test (core + tests build anywhere; the daemon builds on Linux only)
cmake -B build && cmake --build build && ctest --test-dir build

# Without cmake (e.g. on the Mac): compile a test directly
c++ -std=c++17 -Wall -Wextra -Icore/include -Ihal/include core/src/*.cpp tests/test_pgn.cpp -o /tmp/t && /tmp/t

# The daemon on the Mac: a Trixie arm64 container (the CM4's OS/arch; image in .devcontainer/,
# builds kept in the Docker volume aow-hat-build). Needs OrbStack (or Docker Desktop) running.
deploy/dev.sh test                          # build + ctest, incremental
deploy/dev.sh package --version 0.2.20      # signed .aowpkg + .tar.gz into dist/
deploy/dev.sh install dist/aow-hat-0.2.20-linux-arm64.aowpkg   # to the dev CM4's update page
deploy/dev.sh shell                         # builds are in /build/main there, e.g. /build/main/aow-hatd
# VS Code: "Reopen in Container" uses the same image and volume; clangd sees the Linux headers.
```

`tests/pgn_probe.py` stands in for the app (binds UDP 9999, so stop AgOpenWeb first): hello,
scan, then PGN 254 at a set rate, checking every reply and timing the 253.
`tests/gnss_probe.py build/aow-hatd` does the same for GNSS, with a pty as the receiver.
`tests/web_probe.py --port 80` checks every page, API and WebSocket of a running daemon.
`tests/steer_probe.py` closes the steering loop against `--board sim`.
`tests/can_probe.py build/aow-hatd` runs Valtra/MF CAN steering and GVRET on vcan0-2.
`tests/um98x_probe.py build/aow-hatd` runs the GPS page and radio with ptys as the devices.
`tests/imu_probe.py build/aow-hatd` runs an external IMU (BNO085 RVC) on a pty: hello as 121, IMU fields in PANDA.
`tests/ports_probe.py build/aow-hatd` runs the serial ports on ptys: NMEA out (GGA at 5 Hz; the corrected position from PGN 100 and the fallback), RTCM out, and
detection (a radio, unknown text, an IMU, a receiver taking over from a silent onboard one).
The AOW link gateway's own tests (unit, and a vcan probe with the protocol checks) are in
SharpIsoTerm (`gateway/linux/tests`) and run in its CI.
`tests/isobus_probe.py build/aow-hatd` checks the ISOBUS address claim and the tractor ECU role on
vcan1 (stop the service first).
`tests/isobus_tc_probe.py build/aow-hatd build/isobus_implement` runs section control against a
simulated implement on vcan1.
`tests/isobus_vt_probe.py build/aow-hatd build/isobus_vt build/isobus_implement` checks the status
screen for a tractor's terminal against a stand-in terminal on vcan1.
`tests/loop_load.py [secs]` runs on the HAT with CAN2 and CAN3 linked and the service running: the
control loop's timing (`/api/status` `loop`) idle, with CAN3 flooding CAN2, and with every core busy.
On the HAT: `tests/adc_read.py` reads all eight ADC channels, `tests/hat_bench.py` plays the app
with a fixed target angle for a pot-and-meter bench, and `aow-hatd --board hat --indicator-test`
cycles the LEDs and plays the chime (stop the service first for both daemon tests).
`aow-hatd --test-hang-after S` stops the control loop for good after S seconds while the web thread
carries on, so the hardware watchdog resets the CM4. Run it by hand with the service stopped;
never put it in `daemon.env`, or the board resets on every boot.

## Hardware audit scripts

In `hardware/tools/hat-audit/`. They import each other; the plotting ones need `matplotlib`.

```bash
cd hardware/tools/hat-audit
export HAT_SCH=~/Downloads/SCH_AOW-HAT_<date>.json HAT_PCB=~/Downloads/PCB_PCB_AOW-HAT_<date>.json
python3 doccheck.py   # schematic vs HARDWARE_HAT_NETLIST.md
python3 pcb.py        # PCB pad nets vs schematic
python3 conn.py       # routing completeness: nets still split into islands
```

Run all three whenever the user supplies new EasyEDA exports. In `doccheck.py` output, mismatches
on D2/D5/D15-D17/TV1/TV2 `.A`/`.K` and "J8 not in schematic" are pin-naming only, not errors.

## Rules

### Hardware docs
- **The HAT and AiO doc sets are separate and each complete.** No cross-references between
  `hardware/hat/` and `hardware/aio/`.
- **Pin map: two copies, one truth.** `boards/hat/hat_pins.h` and design doc section 7 must change
  in the same commit. The netlist J1 table too.
- **Field-proven simplicity.** Match AgOpenGPS AiO practice; don't over-engineer protection.
- **No switch input on GPIO2 or GPIO3.** The CM4's 1.8 k pull-ups stop a switch behind 1 k from
  reaching a valid low.
- **BZ1 stays outside X 16-62 mm.** It is too tall to sit under the UM982EB.
- EasyEDA editor frame: origin at the board's top-left, Y up, board spans (0,0) to (65, -72.5).
  J1/J2 footprints stay on the top layer; the socket bodies are fitted underneath.

### Daemon
- **Follow the Teensy firmware's code, not its docs.** Its `CLAUDE.md` and `docs/` have stale pin
  tables and rates. Real pins: `lib/aio_config/HardwareManager.h`.
- **The CAN files are the firmware's, near-verbatim** (`core/src/can`, `core/include/aow/can`).
  Teensy parts come from `teensy_compat.h`; mark any other change "AOW HAT" in place.
- **Web pages come from the firmware.** Copy from `web_pages/`, keep the firmware's API field names,
  and note HAT changes in the header comment of each page.
- **The control thread never does I/O for logging**: `log()` queues, the web thread writes out.
  Settings changed by the app are saved by the web thread too (`Shared::requestSave`).
- **Locks shared with the control thread are `PiMutex`** (priority inheritance), never `std::mutex`.
- **Every steering stop path has a test in `tests/test_steer.cpp`.** After changing `core/steer`,
  break the path on purpose once and check a test fails.
- **Core stays free of Linux and hardware headers.** Anything hardware-bound goes behind an
  interface in `hal/`, with the implementation in `boards/hat/`.
- **`STEER_EN` low is the safe state.** Enable, not PWM, is the gate: hardware PWM keeps running if
  the process dies. Every kickout and timeout path gets a unit test before it drives a motor.
- **The wire protocol is fixed.** PGN frames as AgOpenGPS defines them; AiO boards on
  192.168.5.x must keep working exactly as now.
- App-side changes the HAT needs (RTCM target, loopback lock) are app code and go in the
  AgOpenWeb repo, not here.

### What the app expects (from the AgOpenWeb code)
- Sends PGN 254 and 239 at 100 Hz to UDP 8888; listens on 9999.
- Steer module is "lost" after 100 ms of silence: answer each PGN 254 with a PGN 253.
- GPS is stale after 300 ms. It reads `$PANDA` and `$PAOGI`, and also builds a fix from a
  receiver's own sentences (GGA/GNS, VTG, HPR/HDT/THS) for passthrough; an external IMU reaches it
  only in PANDA/PAOGI.
- One daemon answers as steer (126), GPS (120) and IMU (121) on port 8888.

## Licence

Three, one per file (LICENSE, LICENSES/). Every new file starts with its SPDX line and
`Copyright (c) 2026 The AgOpenWeb-HAT contributors`:
- **MIT** for code written here (C++ `//`, scripts `#` after the shebang, Markdown in `<!-- -->`).
- **GPL-3.0-or-later** for anything ported or copied from the Teensy firmware (add
  `Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.`), and for the
  firmware's own files, which keep their notice. Code moved from a GPL file into a new one stays
  GPL. `aow-hatd` as built is GPL-3.0-or-later as a whole.
- **CERN-OHL-P-2.0** for `hardware/` designs; `hardware/tools/` is MIT.
- `deploy/config.txt.hat` has no header: install.sh compares its block line for line.
