# CLAUDE.md - AgOpenWeb HAT

This file provides guidance to Claude Code when working with this repository.

**Key documentation:**
- **[docs/PLAN.md](docs/PLAN.md)** - Software plan: decisions, firmware-to-daemon mapping, safety rules, build phases
- **[docs/PLAN_VT.md](docs/PLAN_VT.md)** - Plan for a virtual terminal service (the implement's screens in AgOpenWeb); nothing built
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
  ptys, and a real TM171 on the dev CM4 (signs checked on the bench). Where each serial
  device is (GNSS receiver, radio, IMU, Keya, and the new NMEA out and RTCM out) is a setting,
  the Device page's port matrix (`ports`; the service passes `--gnss` and `--rs232`); USB
  devices are found by listening (`daemon/usb_scout`). `--radio`, `--keya-serial` and `--imu`
  override the settings. LED and buzzer logic
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
  `third_party/aog-taskcontroller/` is upstream's file **unmodified**: change the stand-in
  headers in `daemon/isobus_compat/` or `daemon/isobus.cpp` instead. Include the stack's
  headers before any `aow/` header and `#undef` its `LOG_*` macros, as `daemon/isobus.cpp` does.
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
  Sync with `rsync -a --exclude build --exclude hardware --exclude .git ./
  chris@192.168.4.217:AgOpenWeb-HAT/`, build there with cmake.

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

# The daemon on the Mac: build and probe it in a Trixie arm64 container (same OS/arch as the CM4)
docker run --rm -v "$PWD":/src:ro debian:trixie bash -c 'apt-get update -qq && apt-get install -y -qq g++ cmake python3 >/dev/null &&
  cmake -S /src -B /b && cmake --build /b -j && ctest --test-dir /b &&
  { /b/aow-hatd & sleep 0.3; python3 /src/tests/pgn_probe.py --rate 100 -n 500; }'
```

`tests/pgn_probe.py` stands in for the app (binds UDP 9999, so stop AgOpenWeb first): hello,
scan, then PGN 254 at a set rate, checking every reply and timing the 253.
`tests/gnss_probe.py build/aow-hatd` does the same for GNSS, with a pty as the receiver.
`tests/web_probe.py --port 80` checks every page, API and WebSocket of a running daemon.
`tests/steer_probe.py` closes the steering loop against `--board sim`.
`tests/can_probe.py build/aow-hatd` runs Valtra/MF CAN steering and GVRET on vcan0-2.
`tests/um98x_probe.py build/aow-hatd` runs the GPS page and radio with ptys as the devices.
`tests/imu_probe.py build/aow-hatd` runs an external IMU (BNO085 RVC) on a pty: hello as 121, IMU fields in PANDA.
`tests/ports_probe.py build/aow-hatd` runs the port settings on ptys: NMEA out (GGA at 5 Hz) and RTCM out.
`tests/isobus_probe.py build/aow-hatd` checks the ISOBUS address claim and the tractor ECU role on
vcan1 (stop the service first).
`tests/isobus_tc_probe.py build/aow-hatd build/isobus_implement` runs section control against a
simulated implement on vcan1.
`tests/isobus_vt_probe.py build/aow-hatd build/isobus_vt build/isobus_implement` checks the status
screen for a tractor's terminal against a stand-in terminal on vcan1.
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
- GPS is stale after 300 ms. Only `$PANDA` and `$PAOGI` are accepted.
- One daemon answers as steer (126), GPS (120) and IMU (121) on port 8888.

## Licence

GPL-3.0-or-later. The daemon ports code from the GPL v3 Teensy firmware, so this is not optional.
