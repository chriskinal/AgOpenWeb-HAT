# AgOpenWeb HAT daemon (`aow-hatd`)

The hardware daemon for the **AOW HAT**, a 40-pin HAT for the Raspberry Pi CM4 IO Board. The HAT
replaces an AgOpenGPS AiO board; this daemon does the work the AiO board's Teensy firmware did:
steering, wheel-angle sensing, GNSS, RTK corrections and tractor CAN.

It talks to [AgOpenWeb](https://github.com/AgOpenGPS-Official/AgOpenWeb) in the AgOpenGPS PGN
protocol over UDP on the loopback interface, so to the app it looks like an AiO board.

**Status: skeleton.** PGN framing and the hardware interfaces exist; nothing drives hardware yet.
The plan is in [docs/PLAN.md](docs/PLAN.md).

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

## Build and test

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

The daemon itself builds only on Linux. Core and the tests build anywhere.

## Licence

GPL-3.0-or-later. Parts are ported from the AgOpenGPS Teensy AiO firmware, which is GPL v3.
