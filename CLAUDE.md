# CLAUDE.md - AgOpenWeb HAT

This file provides guidance to Claude Code when working with this repository.

**Key documentation:**
- **[docs/PLAN.md](docs/PLAN.md)** - Software plan: decisions, firmware-to-daemon mapping, safety rules, build phases
- **[hardware/hat/HARDWARE_HAT_DESIGN.md](hardware/hat/HARDWARE_HAT_DESIGN.md)** - What the board does and why: mechanical, power, circuits, pinouts
- **[hardware/hat/HARDWARE_HAT_NETLIST.md](hardware/hat/HARDWARE_HAT_NETLIST.md)** - Every part and net
- **[hardware/hat/HARDWARE_HAT_ISSUES.md](hardware/hat/HARDWARE_HAT_ISSUES.md)** - Open decisions (H#) and bench checks (B#)

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

- **Board:** routed, audit clean (2026-09-28 export); DRC and Gerber check before ordering.
  4 layers: Top, Inner1 GND plane, Inner2 signal with a GND pour, Bottom. Rules 0.2 mm track and
  clearance, 0.5 mm power. Routed with EasyEDA's JLCPCB router, GND skipped; GND vias placed by
  hand, including one at each J3 protection diode. J1.17 left unconnected (issues H10).
- **Daemon:** runs on Linux and answers the app as steer (126) and GPS (120): hello, scan, and a
  PGN 253 for every PGN 254 (`core/app_link`). GNSS done in core (`nmea`, `gnss`, `nav`,
  `gnss_link`): receiver sentences in, PANDA/PAOGI out, RTCM through; runs today with
  `--gnss /dev/ttyUSB0`. Event logger, JSON settings file and the firmware's web pages
  (Home, Device, Event logger, Logs, Network) on port 80 with WebSockets 8082/8083, served by a
  normal-priority web thread. Steering logic in `core/steer` (arming, kickouts, timeouts, soft
  start, P controller, PGN 250/251/252), closed-loop on `--board sim`; no HAT drivers yet.
  Tractor CAN (all 9 brands, JSON engine, Keya CAN), Keya serial, CAN page and upload, GVRET on
  TCP 23: done and tested on vcan. The firmware's CAN files live in `core/src/can` near-verbatim.
- **Scope:** port every v26 firmware function the HAT hardware supports, web UI included
  (docs/PLAN.md section 2).
- **Dev CM4:** `chris@192.168.4.217`, passwordless sudo, Trixie Lite with the packaged
  PREEMPT_RT kernel. Sync with `rsync -a --exclude build --exclude hardware --exclude .git ./
  chris@192.168.4.217:AgOpenWeb-HAT/`, build there with cmake.

## Layout

```
AgOpenWeb-HAT/
├── core/            # Logic with no hardware or Linux dependency; builds on any desktop
├── hal/             # Interfaces core uses to reach hardware (aow/hal.h)
├── boards/hat/      # HAT pin map (hat_pins.h) and, later, its Linux drivers
├── web/             # The firmware's web pages (web/pages, copied) and API on POSIX sockets
├── daemon/          # The Linux executable: control thread + web thread
├── third_party/     # ArduinoJson 7.4.2 (the firmware's JSON library, MIT)
├── tests/           # Unit tests for core
├── deploy/          # install.sh, aow-hatd.service
├── docs/PLAN.md
└── hardware/
    ├── hat/         # HAT design, netlist, BOM, issues
    ├── aio/         # Earlier AiO board design (parked) + EasyEDA parts and exports
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
