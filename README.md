# AgOpenWeb HAT

The **AOW HAT** board design and its hardware daemon, `aow-hatd`.

The HAT is a 40-pin board for the Raspberry Pi CM4 IO Board. It replaces an AgOpenGPS AiO board,
and the daemon does the work the AiO board's Teensy firmware did: steering, wheel-angle sensing,
GNSS, RTK corrections and tractor CAN.

It talks to [AgOpenWeb](https://github.com/AgOpenGPS-Official/AgOpenWeb) in the AgOpenGPS PGN
protocol over UDP on the loopback interface, so to the app it looks like an AiO board.

**Status.** Board: schematic verified, PCB routing in progress (see `hardware/hat/`).
Daemon: skeleton only; PGN framing and the hardware interfaces exist, nothing drives hardware yet.
The software plan is in [docs/PLAN.md](docs/PLAN.md).

## Two ways to run AgOpenWeb on a Pi

| | Hardware | Steering, GNSS, CAN handled by |
|---|---|---|
| 1 | CM4 + AOW HAT | this daemon |
| 2 | any Pi + an existing AiO board (v4.x / v5.x) on Ethernet | the board's own Teensy firmware |

The installer asks once which one applies. With no HAT the daemon stays disabled.

## Layout

| Folder | Contents |
|---|---|
| `core/` | logic with no hardware or Linux dependency; builds and tests on any desktop |
| `hal/` | the interfaces core uses to reach hardware |
| `boards/hat/` | the HAT's pin map and (later) its Linux drivers |
| `daemon/` | the Linux executable |
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

## Licence

GPL-3.0-or-later. Parts are ported from the AgOpenGPS Teensy AiO firmware, which is GPL v3.
