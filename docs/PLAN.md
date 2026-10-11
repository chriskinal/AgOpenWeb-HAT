# Plan

> Decisions, scope and build order for `aow-hatd`. Written 2026-09-28.
> Source being ported: `Firmware_Teensy_AiO_26` v26.5.5 (Teensy 4.1).
> HAT hardware: `hardware/hat/HARDWARE_HAT_DESIGN.md`.

## 1. Decisions

| Decision | Choice | Why |
|---|---|---|
| Base OS | Raspberry Pi OS Lite, 64-bit | best fit with the Pi overlays and bootloader settings the HAT needs |
| Kernel | PREEMPT_RT, Raspberry Pi's packaged `linux-image-rpi-v8-rt` (`kernel=kernel8_rt.img` in `config.txt`) | a bounded time to switch the motor off; the packaged build tracks the stock kernel, so nothing to build |
| Protocol to the app | AgOpenGPS PGN over UDP, loopback | the app treats the HAT like an AiO board; existing boards keep working |
| Teensy firmware | unchanged | it stays on the AiO boards people own |
| HAT present? | asked once at install | reliable; the HAT has no ID EEPROM |
| Repo | this one, separate from AgOpenWeb | |
| Language | C++17 | the firmware logic ports with few changes |

## 2. What comes from the firmware

**Scope: everything the HAT's hardware supports.** The CM4 + HAT replaces the AiO board and also
runs AgOpenWeb; users reach both over Wi-Fi. So the daemon carries the firmware's functions,
including its web pages, not a trimmed subset. A module is left out only if the HAT lacks the
hardware or Linux genuinely does the job.

Follow the firmware **code**, not its docs: `CLAUDE.md` and `docs/` there have stale pin tables
and rates. Line counts are the firmware's (.h + .cpp).

### 2.1 Ported

| Firmware module | Lines | Here | Notes |
|---|---|---|---|
| `PGNProcessor`, `PGNUtils` | 350 | `core/pgn`, `core/app_link` (done) | hello, scan, 253 per 254 |
| `GNSSProcessor`, `NAVProcessor`, `NMEAMessageBuilder`, `calc_crc32` | 2,800 | `core/nmea`, `core/gnss`, `core/nav` (done) | one GNSS UART; the u-blox GPS2/UBX path drops (the UM982 gives heading) |
| `RTCMProcessor` | 210 | `core/gnss_link` (done) | UDP 2233 and the radio (`--radio`, RS-232 #1) to the GNSS UART |
| `EventLogger` | 600 | `core/log` + `daemon/web_thread` (done) | same levels, sources and rate limits; console → journald, UDP syslog, ring buffer, WebSocket 8083 |
| `ConfigManager`, `EEPROMLayout` | 1,040 | `core/settings`, `daemon/shared` (done) | `/etc/aow-hat/settings.json`, written atomically; same fields and defaults |
| `SimpleHTTPServer`, `SimpleWebSocket`, `SimpleWebManager`, `LogWebSocket`, `TelemetryWebSocket` | 3,000 | `web/server`, `web/api` (done for Home, Device, Event logger, Logs, Network) | same routes; ports 80, 8082, 8083 so the pages work unchanged |
| `web_pages/*` (the served set, ~168 KB) | | `web/pages/` (10 of 11: the update page left) | copied as-is; HAT changes marked in each file |
| `AutosteerProcessor` | 1,650 | `core/steer` (done, including CAN valve gating and engage buttons) | 100 Hz, P controller, min/high PWM, soft start, Ackermann, engage by switch or button, valve-ready gating |
| `KickoutMonitor` | 700 | `core/steer` (done: current, CAN, motor error) | current-sense, CAN and Keya kickouts; PGN 250 from current |
| `ADProcessor` | 840 | `core/steer` (done) + HAT ADC driver | WAS and current as the firmware; work switch digital only; adds remote, supply and key sense |
| `PWMMotorDriver` | 340 | HAT driver | rewritten for PWM + DIR + EN; brake mode only where the driver supports it |
| `MotorDriverManager`, `KeyaSerialDriver` | 590 | `core/motor` (done), `--keya-serial` | Keya over serial, `E2` probe |
| `TractorCANDriver`, `CANProtocolEngine`, `CANConfigParser` | 2,460 | `core/src/can` (done, near-verbatim via `teensy_compat.h`) + SocketCAN | all 9 brands, JSON brand files (`deploy/can-config`), Keya CAN, CAN wheel angle, CAN engage buttons, CAN kickout |
| `GVRETServer` | 390 | `core/gvret` + web thread (done) | SavvyCAN on TCP 23 |
| `MessageBuilder` | 140 | `core/steer_pgn`, `AppLink::sendPopup` (done) | PGN 221 popups |
| `LEDManagerFSM` | 650 | `core/indicators` (done) + SK6812 driver | PWR, GPS, STEER, INS on the 4 LEDs; RTCM and button pulses |
| `HardwareManager` buzzer | | `core/indicators` (done) + piezo driver | the boot chime by volume setting |
| `UM98xManager` | 645 | `core/um98x` + GPS page (done) | read/write receiver CONFIG, MODE, logs; the control thread lends the GNSS port |
| `CommandHandler` | 300 | `deploy/aow-hatctl` (done) | status, logs, log level, rate limit, buzzer, CAN, GPS, restart, over the local API |
| `Version` | | `core` | |

### 2.2 Replaced by Linux

| Firmware | Replaced by |
|---|---|
| `SimpleScheduler`, `main.cpp` loop | the daemon's poll loop and timerfd |
| QNEthernet, `QNetworkBase`, `QNEthernetUDPHandler` | sockets; the HAT talks to the app on loopback. The Network page shows the CM4's addresses; PGN 201 subnet change does not apply |
| DHCP server (DHCPLite) | NetworkManager shared mode on `eth0`, set up by the installer, if machine modules hang off the CM4's Ethernet |
| `SimpleOTAHandler` (Intel-HEX to flash) | an update page that takes a release package (later) |
| `CANGlobals`, `CANManager` (FlexCAN) | SocketCAN `can0`-`can2` |
| `CANConfigStorage` (LittleFS) | `/etc/aow-hat/can_config.json` |
| `SerialManager` | termios |

### 2.3 No hardware on the HAT

Section and hydraulic outputs (`MachineProcessor`), a separate IMU (`IMUProcessor`, `I2CManager`),
the steering-wheel encoder (`EncoderProcessor`), the pressure sensor, JD PWM, the analog work
switch and its page, the speed pulse (`PWMProcessor`), the Danfoss valve, the ESP32 bridge, the
second GPS UART. Sections stay on an external machine module or go over ISOBUS (section 8).

### 2.4 Serial ports

| Firmware | Role | HAT |
|---|---|---|
| Serial5 `SerialGPS1`, 460800 | GNSS in, RTCM and UM98x commands out | GNSS UART (UART5) |
| Serial3 `SerialRadio`, setting (115200) | RTK radio in, forwarded to GNSS | RS-232 #1 (UART2) |
| Serial7 `SerialRS232`, 115200 | Keya serial motor | RS-232 #2 (UART3) |

### 2.5 Where the port differs from the firmware, on purpose

- PANDA/PAOGI numbers are rounded, not truncated (the firmware prints HDOP 0.9 as `0.8`), latitude
  and longitude are formatted from doubles, and the time is zero-padded to `hhmmss.s`.
- GNS differential age is read from field 11; the firmware reads field 12, the station ID.
- HPR age stays in seconds; the firmware multiplies it by 100.
- Sentences with no checksum are dropped; the firmware parsed them unchecked.
- GPS passthrough (raw NMEA to the app, a Device page option) is kept for AgIO users, but AgOpenWeb
  accepts only PANDA and PAOGI, so with it on AgOpenWeb loses GPS. The page says so. Only
  checksum-valid sentences are passed on (the firmware passed everything).
- UM98x setup no longer freezes steering: the firmware blocked its whole loop for the seconds it
  talks to the receiver; here the web thread does it while the control loop runs on, with the
  GNSS stream (and RTCM into the receiver) paused.
- Firmware bugs not carried over: the language byte and the CAN config both live at EEPROM 900, so
  a language click wipes the CAN config (settings are separate JSON keys here; the unused language
  setting is dropped); PGN 253 bit 2 (remote) is always 0 (wired to the remote input here); the
  steer hello carries a fixed crc (computed here).
- Steering: guidance on/off edges are kept until the next control tick (the firmware kept one
  flag per PGN 254, so at 100 Hz a second frame in the same tick erased a toggle); the soft-start
  reversal check compares values before motor direction is applied (the firmware compared the
  inverted output, which misfires with the motor reversed); `Kp x error` is clamped before its
  int16 cast; the work switch honours Invert Work Switch (the firmware only did for the analog
  switch); the Ethernet-link check drops (loopback). Kept as in the firmware: the 2 s steer
  timeout, the 2 s kickout cooldown, the 300 ms guidance-off disarm, arming by guidance-on even in
  switch mode, and a kickout that clears itself on the next cycle once the motor is stopped.
- CAN: the app-button handshake (arm briefly when the valve is not ready, then disarm) lasts
  its intended 200 ms with the motor held off; in the firmware its own "not ready while armed"
  check cut it to one tick with a second popup. The switch-mode "not ready" popup shows once per
  attempt, not every 10 ms. GVRET also mirrors the JSON engine's frames (the firmware mirrored
  only the driver's own reads and writes).
- Firmware dead code not carried over: `CANFilterHelper` (SocketCAN filters instead, if needed),
  `KeyaCANDriver` (Keya runs through the tractor CAN driver, as in the firmware),
  `WheelAngleFusion` (can never initialise as shipped), the CAN `work[]` rules (never parsed).

## 2A. Daemon structure

- **Two threads.** The control thread (SCHED_FIFO) owns every device: UDP to the app, GNSS, CAN,
  ADC, motor, LEDs. The web thread (normal priority) serves HTTP and the WebSockets and never
  touches hardware.
- **Between them:** the control thread publishes a status snapshot (under a mutex, copied, never
  held across I/O); the web thread sends commands (new settings, CAN config, UM98x read/write,
  restart) through a queue with an eventfd the control loop polls.
- **JSON:** ArduinoJson 7.4.2, the firmware's own library (MIT), vendored in `third_party/`, so the
  CAN config code ports nearly line for line.
- **Files:** settings and the CAN brand file in `/etc/aow-hat/`; web pages compiled in, as in the
  firmware.

## 3. Timing the app expects

- The app sends PGN 254 and 239 at 100 Hz to port 8888 and listens on 9999.
- It treats the steer module as lost after 100 ms of silence. The firmware answers each PGN 254
  with a PGN 253; do the same.
- GPS is stale after 300 ms. Only `$PANDA` and `$PAOGI` are accepted.
- One daemon answers as steer (126), GPS (120) and, if IMU data exists, IMU (121), all on port 8888.

## 4. Safety

- **`STEER_EN` low is the safe state.** The daemon holds it; the service unit forces it low on
  any exit; the pin is pulled down at boot.
- **Hardware PWM keeps running if the process dies**, which is why enable, not PWM, is the gate.
- **The HAT watchdog (1.6 s) resets the CM4** if the daemon stops kicking it. Enable it only once
  the control loop is running.
- **Steer timeout.** The firmware stops the motor 2 s after the last PGN 254. On one board with a
  100 Hz stream that can be far shorter; pick the value during phase 3 and test it.
- Every kickout and timeout path gets a unit test before it drives a motor.

**Only the paired app is heard (2026-10-10).** On the bench a second AgOpenWeb on the LAN
(192.168.4.127) was broadcasting PGN 254 with guidance off to the subnet, and the daemon took it
as its own app: a 10 ms disarm and re-arm every 17 s. Two tractors on one Wi-Fi would do the
same to each other. The daemon now drops datagrams from any host but `--app` (loopback by
default), counts them, and logs a warning naming the sender. `--any-app` turns the filter off
for bench use.

## 5. Changes needed in AgOpenWeb

| Change | Where | Must not break |
|---|---|---|
| RTCM forward target configurable (was `192.168.5.255:2233`). **Seen working 2026-10-10:** the nightly sends RTCM to loopback when the HAT is its module; check the code path and whether anything is left to do | `MainViewModel.Ntrip.cs:110-111` | AiO boards on 192.168.5.x |
| Lock to a loopback module, so 100 Hz packets stop being broadcast on every interface | `UdpCommunicationService` `LockToSubnet` | discovery of LAN modules |
| Service unit: start after `aow-hatd` when a HAT is fitted | `deploy/linux/agopenweb.service` | installs with no HAT |
| ISOBUS (section 8), optional: send PGN 241 (section control on/off) and read PGN 240 (the implement's section count and reported states), as AgOpenGPS does for AOG-TaskController | `PgnBuilder`, section control | machine modules on PGN 239/229 |

## 6. Build order

Hardware-free work went first, while the board was being made. The HAT arrived 2026-10-09.

| Phase | Work | Needs the HAT | Done when |
|---|---|---|---|
| 1 | OS bring-up: RT kernel, overlays, `hatcan1`-`hatcan3`, UARTs, ADC, PWM, watchdog, key-off monitor (**done 2026-10-10**, record below) | yes | each device answers from the shell |
| 2 | GNSS: receiver in, PANDA/PAOGI out, RTCM through (done; UM982 on the HAT's UART5, position and heading shown in AgOpenWeb 2026-10-10) | UART only | AgOpenWeb shows an RTK fix from the HAT (**done 2026-10-10**) |
| 3 | Foundation: event logger, settings file, web server with Home, Device, Event logger and Logs pages, telemetry (done) | no | pages work on the dev CM4 |
| 4 | Steering in core: PGN 251/252, controller, soft start, kickouts, timeouts, PGN 250 (done; `--board sim` closes the loop on a bare CM4); the HAT drivers (done 2026-10-10, loop closed with a pot as the WAS) | drivers only | every stop path unit-tested (done, mutation-checked); bench motor follows the app (open: needs a motor driver) |
| 5 | CAN: SocketCAN, JSON engine, the 9 brands, Keya CAN and serial, CAN page and upload, GVRET (done on vcan) | bench tests only | one brand verified on a machine |
| 6 | UM98x page, radio RTCM on RS-232 #1 (done, on ptys) | receiver | receiver config read and written from the page (**done 2026-10-10** on a UM982; radio RTCM not yet tried with a radio) |
| 7 | LED and buzzer logic, `aow-hatctl`, installer, LED and piezo drivers, update page (done); the AgOpenWeb changes | yes | clean install on a fresh image, both hardware choices |
| 8 | Latency measurement under load | yes | worst case recorded; decide if anything more is needed |
| 9 | ISOBUS: section control task controller inside the daemon (section 8; planned, not started) | CAN channel | an ISOBUS implement's sections follow AgOpenWeb |

### Phase 1 record

| Date | Check | Result |
|---|---|---|
| 2026-09-28 | RT kernel on a CM4 Rev 1.1 (2 GB), Trixie Lite, on Wi-Fi | `6.18.50+rpt-rpi-v8-rt` boots; `config.txt.pre-rt` kept as the fallback |
| 2026-09-28 | `cyclictest -m -S -p90 -i1000 -D60`, idle | max 42 µs, avg 9-10 µs |
| 2026-09-28 | `aow-hatd` at FIFO 80, `pgn_probe.py` 2000 x PGN 254 at 100 Hz | 2000/2000 answered, round trip median 0.18 ms, worst 0.35 ms |
| 2026-09-28 | `gnss_probe.py` (pty as the receiver) | GGA -> PANDA 20/20, HPR -> PAOGI, RTCM 2233 -> serial byte-identical |
| 2026-09-28 | `steer_probe.py`, service with `--board sim` | armed by guidance on, motor within 100 ms, wheel at 5 deg in 0.81 s and holding; 2 s timeout logged; PGN 252 gains saved |
| 2026-09-29 | `um98x_probe.py` (ptys as receiver and radio) | page read in 0.3 s and write ending in SAVECONFIG, PANDA resumes after each; radio RTCM reaches the receiver byte for byte |
| 2026-09-29 | `can_probe.py` on vcan0-2, Valtra/MF legacy code | valve commands 50 Hz with intent 252; PGN 253 angle from the CAN curve; engaged: intent 253 and PWM x 128; valve not ready: popup and back to 252; GVRET streams every frame once, 20 ms apart |
| 2026-10-10 | **First boot on the HAT** (fresh Trixie Lite, `linux-image-rpi-v8-rt` 6.18.50, `boards/hat/aow-hat-overlay.dts` + `deploy/config.txt.hat`) | all three MCP251863s probe over SPI0 (`MCP2518FD rev0.0`, 40 MHz, 20 MHz SPI); UARTs `/dev/ttyAMA2` (RS-232 #1), `/dev/ttyAMA3` (RS-232 #2), `/dev/ttyAMA5` (GNSS), console on `/dev/ttyAMA0`; PWM0 on `/sys/class/pwm/pwmchip0/pwm0`; ADC on `/dev/spidev0.3` |
| 2026-10-10 | `tests/adc_read.py` (spidev, mode 3, 1 MHz) | IN2 `VIN_SENSE` 727 counts, IN4 `KEY_SENSE` 721, grounded inputs 0; WAS and current inputs float. With the reference taken as 5.00 V that is 11.7 V; the Fluke read the supply at 11.98 V and `5V_MAIN` at **5.12 V**, which makes the ADC exact (727 x 5.12 / 4096 / 0.0758 = 11.99 V). The daemon must use the real rail voltage for VIN and key, not 5.00 V; WAS and current are ratiometric and unaffected |
| 2026-10-10 | CAN internal loopback, 250 kbit/s, `cansend`/`candump` on each interface | all three send and receive, 0 errors. The kernel names them in probe order (spi0.2 came up first), and renaming to `can0..2` fails because those names are already taken by the kernel's own choice; `deploy/80-aow-hat-can.rules` names them **`hatcan1`, `hatcan2`, `hatcan3`** by chip select (CAN1-3 on J3), and the service unit passes `--can hatcan1,hatcan2,hatcan3` |
| 2026-10-10 | `cyclictest -m -S -p90 -i1000 -D20` on the HAT, idle | max 31 µs, avg 8 µs |
| 2026-10-10 | **Watchdog U10**: GPIO3 low (enable), GPIO6 kicked at 2 Hz for 5 s, then left | CM4 reset within seconds of the last kick and rebooted on its own (the key was on, so the latch held); back on the network with uptime 17 s when checked. GPIO3 is back to input/pull-up after the reset, so the watchdog is off until the daemon enables it |
| 2026-10-10 | **`aow-hatd --board hat`** (`boards/hat/hat_board.cpp`: ADC over spidev, switches and DIR/EN/WDT through the GPIO character device, PWM0 through sysfs at 20 kHz), watchdog on, `pgn_probe.py` 300 x PGN 254 at 100 Hz | 300/300 answered, round trip median 0.08 ms, worst 0.48 ms; WAS reads the floating input (-4.6 deg); EN low from start to stop; the watchdog is enabled at the first tick, kicked at 10 Hz, and set off (GPIO3 high) at exit |
| 2026-10-10 | **Steering bench**, `tests/hat_bench.py` (the app at 100 Hz, Kp 40, target +20 deg) with a 10 k pot on J3.3/J3.4/J3.5 as the WAS and the Fluke on J3.10/11/12 | pot reads -68 to +68 deg lock to lock (30 counts/deg); PWM 255 away from the target, 28 at 0.5 deg; J3.12 EN 3.3 V while armed, J3.10 3.3 V at full PWM, J3.11 DIR 0 V below the target and 3.3 V past it; no disarm or kickout over several sweeps. Killing the script mid-run (the app goes silent): the 2 s communication timeout logged, J3.12 EN low and PWM duty 0 on the hardware. (First runs showed a 10 ms disarm every 17 s: a tablet on the LAN broadcasting PGN 254 with guidance off; the daemon now ignores hosts other than `--app`, section 4) |
| 2026-10-10 | RS-232 #1 loopback through U9 (J3.23 to J3.24), 1024 bytes each way on `/dev/ttyAMA2` | identical at 9600, 115200 and 460800 baud. RS-232 #2 (`/dev/ttyAMA3`, J3.25 to J3.26) the same |
| 2026-10-10 | **GNSS board fitted: UM982** (firmware R4.10Build13495), on UART5 `/dev/ttyAMA5` at 460800 | receiver sends GGA, VTG and HPR at 10 Hz; the daemon counted 3596 sentences, 0 bad checksums, 1198 nav sentences sent, 27 satellites, dual-antenna heading valid (HPR quality 4). Commands go the other way too: `VERSIONA` and `CONFIG` answered. No corrections fed, so fix quality 1 |
| 2026-10-10 | **AgOpenWeb on the same CM4**: nightly f7ab69da, headless bundle `agopenweb-linux-arm64.tar.gz`, `install.sh --from app`; UI on :5174, app on UDP 9999, daemon on 8888 and 2233 | the app sends each frame on loopback and as a broadcast on wlan0; the daemon hears the loopback copy (3202 PGN 254 in the first minute, 0 rejected, answers on loopback) and drops the broadcast copy with its sender filter; PWR LED green. **In the app's UI:** the HAT shows as the GPS and autosteer modules, position and dual-antenna heading from the UM982 are live, and the wheel sprite follows the pot on the WAS input |
| 2026-10-10 | **RTCM end to end**: NTRIP configured in AgOpenWeb (nightly f7ab69da) on the same CM4 | the app sends RTCM to `127.0.0.1:2233`; the daemon counted 1284 packets, 0 bytes dropped, and pulses the GPS LED blue; the UM982 went from fix quality 1 to **5 (RTK float)** with a correction age of 0.2 s, and then to **4 (RTK fixed)**: the GPS LED went green. This is phase 2's "done when" |
| 2026-10-10 | **System update**, through `/api/ota/upload` on the dev CM4, packages from `deploy/package.sh` | 0.1.0 to 0.1.1: verified, installed, checked, done in about 20 s, `daemon.env` kept. One byte flipped: refused, "signature does not match". Signed with another key: refused. Random bytes: 400 at the daemon. A correctly signed 0.1.2 whose daemon exits at once: detected, 0.1.1 put back, no watchdog reset, `STEER_EN` low throughout. Back to 0.1.0 through the same path. Unit test covers the 409 while steering is engaged. **From a browser** on the `/ota` page: 0.1.1 chosen and installed, every step shown, ending in "Update complete" |
| 2026-10-10 | PWM0 at 100 Hz, 50 %, metered at J3.10 | 1.647 V on the Fluke (3.3 V x 0.5); J3.11 DIR and J3.12 EN at 0 V from boot |
| 2026-09-28 | `install.sh --hat` on the dev CM4, `web_probe.py` on port 80 | control thread FIFO 80, web thread normal; all pages and APIs; telemetry 101 Hz; log WebSocket; settings saved atomically to `/etc/aow-hat/settings.json`; Restart button -> systemd restart; journal priorities work |

## 7. Open items

**Status 2026-10-10.** The HAT is fitted to the dev CM4 and every function on it has working
code: phases 1 to 7 are done except the items below. Each needs a part on the bench, a machine,
or a decision.

In order:

1. **A motor driver on the bench** (MD13S / IBT-2 class). Settles three things:
   - PWM frequency. The default is 20 kHz (`--pwm-hz`); the firmware used 75 Hz for the DRV8701.
   - Current sense scale. `kCurrentCountsPerVolt` in `core/steer.h` maps HAT volts to the
     firmware's Teensy counts so the app's current threshold keeps its meaning; set it from the
     driver's current output.
   - The steer timeout. It stays at the firmware's 2 s. AgOpenWeb sends PGN 254 at 100 Hz, so a
     shorter value is possible, but the app has known UI stalls of 1-3 s (its own comments), and
     the motor would stop mid-turn on every stall.
2. **GNSS.** The UART5 path is proven both ways with a UM982, and RTCM from the app reaches
   it: RTK fixed on the bench (record above). The UM98x page reads the real receiver's
   configuration and writes it back; the receiver restarts on the save and comes straight back
   (2026-10-10). Still open: one INSPVAXA capture from a **UM981** to settle the field numbers.
   The port keeps the firmware's (velocities at 15-17), but Unicore's layout has undulation
   after height, which would put them at 16-18 and make the speed wrong. The UM982 has no INS,
   so it cannot answer that.
3. **CAN on a real bus.** Two channels linked carry traffic cleanly (section 8, real CAN
   record), but a channel stayed dead after repeated bus-off until the interface was cycled:
   fix that first. Loopback passed. Still to check on the MCP251863s: that
   `restart-ms 100` brings a bus back after bus-off (two ports wired together, SJ1-SJ3 closed),
   then one brand on a machine.
4. **The System Update page: built 2026-10-10**, a signed upload page, because users, not only
   the developer, will update units, often with no internet at the tractor.
   - `deploy/package.sh` (Linux arm64) builds the bundle and signs it: `.tar.gz` for a first
     install, `.aowpkg` (the tar.gz, a 64-byte Ed25519 signature, `AOWHAT1\n`) for the page.
   - The page (`/ota`, the firmware's, adapted) posts the file to `/api/ota/upload`. The daemon
     only takes delivery: it refuses while steering is engaged and refuses a file that is not
     package-shaped, then starts `aow-hat-update.service`.
   - `deploy/aow-hat-update` (root, its own unit) checks the signature against
     `/usr/local/lib/aow-hat/update-key.pem`, runs the bundle's `install.sh`, then requires the
     new daemon to stay up for 12 s, report the new version and answer `/api/status`. If not,
     it reinstalls the stored previous bundle. Progress is in `/api/ota/status`.
   - Per-unit options (`--adc-ref`) moved to `/etc/aow-hat/daemon.env`, which updates never
     touch. `install.sh` replaces the config.txt block and the overlay only when they changed,
     and the page then offers a unit restart.
   - **The HAT page updates the HAT software only.** AgOpenWeb's updates are AgOpenWeb's.
   Still to do:
   - **The release key.** `deploy/update-key.pem` is a development key whose private half is on
     the developer's machines (`~/.config/aow-hat/`). Replace it when releases move to the
     AgOpenGPS official organisation; units carrying the development key need one last
     install by hand to switch.
   - A CI workflow that runs `package.sh` and publishes the two files (waits for the public repo).
   - Old signed packages install too: there is no downgrade protection. Decide if it matters.
5. **The AgOpenWeb changes** (section 5), in that repo.
6. **Latency under load** (phase 8), with GNSS, CAN and the web pages all busy.

Smaller:

- The brand JSON files are in `deploy/can-config/`; the upload page takes a file from the
  user's device. Decide whether the installer puts them somewhere the page can offer directly.
- `--adc-ref` in the service unit is this unit's measured `5V_MAIN` (5.12 V). It only scales the
  VIN and key telemetry; decide whether it becomes a setting on the Device page.
- Single-feed detection (sense wire and key on the same switched 12 V) is logged by the key
  monitor at key-off; the warning at the next boot is not written.

Settled on the board, 2026-10-10 (details in the phase 1 record):

- ADC path: `spidev` (`/dev/spidev0.3`). The Pi kernel has no `ti-adc128s052` module.
- LED timing under the RT kernel: not a scheduling question. The frame is loaded whole into the
  PCM FIFO and shifted out by hardware. Needs `/dev/mem`, so root.
- `pinctrl` for the service unit's stop action: present as `/usr/bin/pinctrl` on Trixie and used.
- Device names: GNSS `/dev/ttyAMA5`, RS-232 #1 `/dev/ttyAMA2`, RS-232 #2 `/dev/ttyAMA3`, CAN
  `hatcan1`-`hatcan3`.

The dev CM4 (`chris@192.168.4.217`) runs `aow-hatd --board hat` and `aow-keymon` as services,
installed by `deploy/install.sh --hat`. Its `~/AgOpenWeb-HAT` is an rsync copy, not a git
checkout.

## 8. ISOBUS (phase 9, planned)

Decided 2026-10-10. Nothing here is built.

**Why now.** The Teensy had too little memory for an ISOBUS stack. The CM4 has about 1.5 GB
free with the daemon and AgOpenWeb running; a stack with a large implement pool needs a few MB.
The RT kernel ships `can-j1939`, and ISOBUS is 250 kbit/s extended-frame CAN, which every HAT
channel does.

**Scope, in order.**

1. **Stack on one channel**: address claim, and the tractor ECU role (speed, NMEA 2000 course
   and speed) when the tractor has none.
2. **Task controller server, section control.** The main deliverable.
3. **Curvature steering** as one more steering output, off by default. Tractors that take the
   ISOBUS guidance command want either a bought unlock or an authenticated (TIM) session. Nobody
   has the certificates, so this covers unlocked tractors and open valves only: uncommon.
4. Virtual terminal server (the implement's screen in AgOpenWeb): not planned. It is mostly app
   UI work; AgOpenWeb's `Plans/ISOBUS_VT_FRONTEND.md` discusses it.

**Source.** AOG-TaskController (AgOpenGPS-Official, GPL-3, `/Users/chris/Code/AOG-TaskController`)
on AgIsoStack++ (MIT). It already runs headless on SocketCAN and does section control only.

- Take `task_controller.cpp/.hpp` (about 1,200 lines, depends only on AgIsoStack++)
  near-verbatim, under the same rule as the firmware's CAN files: changes marked "AOW HAT" in
  place, so upstream fixes merge by hand without a fight.
- Take what is needed from `app.cpp`: control functions, tractor ECU, the status messages.
- Leave out its UDP layer, its settings file, its Boost and nlohmann dependencies and the
  Windows CAN drivers. Its own virtual terminal status page: decide when building.
- Pin AgIsoStack++ to the commit upstream pins.

**Why inside `aow-hatd`, not its own service.** Run separately it binds UDP 8888, which the
daemon already holds; it picks its network interface by a `192.168.5.x` match and broadcasts
there; and it has no UI off Windows. Inside the daemon the section states and speed are
already in memory, the settings and web pages are the daemon's, and it ships in the same update
package. A crash in the stack takes steering down with it until systemd restarts the daemon.
That is accepted: if the implement side has failed there is no reason to keep steering.

**Structure.**

- AgIsoStack++ runs its own normal-priority threads and opens its channel itself. The control
  thread never calls into it and never waits on its locks: section states and speed go across
  in a small snapshot under a `PiMutex`, status comes back the same way.
- One channel is set as the ISOBUS channel in settings (default the second, the firmware's
  `ISO_Bus`). A brand file may still use that channel: SocketCAN allows both on one interface.
- Leave the channel's 120 ohm jumper open on a tractor's ISOBUS; the bus is terminated already.
- Section states come from the PGNs AgOpenWeb sends now: 16 sections in PGN 254 and 239, 64 in
  PGN 229 when more than 16 are configured. No app change for on/off control.
- A new ISOBUS page (not from the firmware): channel, tractor ECU on or off, address claims,
  implement name, section count and width, live section states.
- Curvature steering, when built, sends from the control thread with the stack's claimed
  address; the tractor's measured curvature stands in for the WAS and its "not accepting" state
  is a new stop path, with a test.

**Step 1 record (2026-10-10, branch `isobus`).** The stack is in the daemon and claims an address.

- `daemon/isobus.cpp`: starts AgIsoStack++ on the channel set in `isobus.enabled` and
  `isobus.channel` (default off, CAN2), claims as a task controller at address 247, and
  publishes state, address and NAME in `/api/status` under `isobus`. The NAME's identity
  number comes from the unit's machine id, so two HATs on one bus differ.
- Its threads run at normal priority while the control thread is SCHED_FIFO 80 (checked with
  `chrt` and `ps -L`).
- `tests/isobus_probe.py` on vcan: claims 247 with the right NAME, answers a request for
  address claimed, and moves to 128 when a higher-priority NAME takes 247.
- On the real `hatcan2` with nothing connected the claim still "succeeds", because nothing
  objects. `busActive` in the status turns true only once a frame of ours is acknowledged,
  and the log warns after 5 s without that.
- Cost on the CM4: the stack takes about 4.5 minutes to build the first time; the binary grew
  from 0.7 to 1.0 MB; resident memory 39 MB with ISOBUS running against 23 MB without.
- AgIsoStack++ is fetched by CMake as a tarball of the pinned commit (no git needed);
  `-DAOW_ISOBUS=OFF` builds without it. Its logging macros share names with the daemon's:
  see the include order at the top of `daemon/isobus.cpp`.
- Not yet tried against a second real CAN node.

**Step 2 record (2026-10-10, branch `isobus`).** Section control works against a simulated implement.

- `third_party/aog-taskcontroller/`: upstream's `task_controller.cpp/.hpp` at commit `4ede403`,
  **unmodified**. The two headers it expects are the daemon's own, in `daemon/isobus_compat/`:
  its log lines go to `isobus/tc.log` beside the settings file (and to the logger at debug
  level), and implements' descriptions are saved under `isobus/<NAME>/`.
- `daemon/isobus.cpp` does what upstream's `app.cpp` did: starts the server 250 ms after the
  claim, announces basic and section control (1 boom, 64 sections), sends the task controller
  status every 2 s, and passes section states each way.
- **Where the sections come from.** `AppLink::sections()`: the 16 bits in PGN 254, or all 64 from
  PGN 229 while the app sends it. The control thread writes them to two atomics each tick; the
  ISOBUS thread reads them every 10 ms. No lock between the two.
- **When section control is on.** Upstream waits for PGN 241 from the app. AgOpenWeb does not
  send it, so the rule here is: on while the app's PGN 254 is arriving (the 2 s app timeout),
  unless an app has sent PGN 241 = 0. Going off, every section is set off first. Stopping the
  daemon does the same.
- `tests/isobus_tc_probe.py` with `build/isobus_implement` (AgIsoStack++'s section control
  simulator, 16 or 64 sections) on vcan: connect, sections from PGN 254 and PGN 229 reaching
  the implement, its reported states coming back in `/api/status`, PGN 241 off and on, app
  silent (sections off after 2.0 s), implement lost and replaced, sections off when the daemon
  stops. Section commands reach the implement in 40 to 85 ms, of which 20 ms is the test's
  own polling.
- **An upstream cost to raise with them.** `ClientState::get_section_actual_state()` searches
  the whole description for each section's parents: 2.3 ms a call with a 64-section implement
  on the CM4, 150 ms for all 64. Upstream calls it for every section ten times a second, which
  would take a whole core on a Pi. Here `TcServer` (a subclass in `daemon/isobus.cpp`, upstream's
  file untouched) notes in `on_value_command()` which sections an implement has just reported
  on, and only those are read again, one every 10 ms from a copy, so setpoints are never held
  up. ISOBUS thread CPU with an implement connected and idle: 1.2 %, the same for 16 and 64
  sections (it was 94 % for 64 before this).
- Upstream uses `std::find_if` without `<algorithm>`; the build adds the include from outside.
- The first configure sets the build type to Release if none is given.

**Step 3 record (2026-10-10, branch `isobus`).** The tractor ECU role and the ISOBUS page.

- **Tractor ECU role** (`isobus.tractorEcu`, default on): two seconds after the claim, if no
  other device on the bus is a tractor ECU, the daemon claims the fixed address 240 and sends
  wheel, ground and machine-selected speed, the NMEA 2000 speed message and J1939 PGN 65256,
  as upstream does. Unlike upstream it never starts beside a real tractor ECU, and it stops
  if one appears: taking 240 from the tractor's own would put our speed in place of the
  tractor's.
- Speed is the app's (PGN 254), or GNSS speed with no app. Direction is always forward: PGN 254
  has no sign. Distance is summed in the daemon. No course is sent.
- `tests/isobus_probe.py` now covers it: the 240 claim, 5.0 km/h from the app arriving as
  1389 mm/s on the bus, stopping when a tractor ECU appears, never claiming with one there
  from the start, and the setting off.
- **ISOBUS page** (`/isobus`, a tile on the home page, `web/pages/IsobusPage.h`, the HAT's own):
  the three settings, bus state, tractor ECU state and speed, the implement's name, section
  count and width, and a box per section coloured by asked-for against reported. Settings are
  read at startup, so a save offers to restart the daemon. `/api/isobus` serves it.
- Checked in a browser against the simulated implement.

**Real CAN record (2026-10-10).** Version 0.2.0 installed on the dev CM4 through the update
page's path. CAN2 and CAN3 linked at J3 (pin 17 to 19, pin 16 to 18), SJ2 and SJ3 closed; the
daemon's task controller on `hatcan2`, the simulated implement on `hatcan3`.

- The implement connects, and section patterns from PGN 254 reach it in 42 to 63 ms. Its
  reported states come back, speed from the tractor ECU role is on the bus (8.0 km/h), and all
  sections go off 2.0 s after the app stops. No bus errors on either channel in about 2,000
  frames each way. This is also the first traffic between two of the HAT's CAN transceivers.
- **A channel did not come back by itself after bus-off.** While the link was first wired
  crossed, `hatcan2` went bus-off about 20 times (`restart-ms 100` restarting it each time).
  Once the wiring was right it sat error-active with a full transmit queue: every send failed
  with "No buffer space available" and nothing went out, until the daemon was restarted, which
  takes the interface down and up. Seen once. Until it is understood, a bus fault on a real
  machine could leave a channel dead until a restart: the daemon should notice a channel
  whose sends keep failing and cycle the interface. This is the bus-off check in section 7
  item 3, and it applies to the brand CAN channels as much as to ISOBUS.

**Stalled-channel recovery (2026-10-10, version 0.2.1 on the dev CM4).**

- `daemon/can_watch.cpp`, its own normal-priority thread, looks at each real CAN channel once
  a second. The rule (`core/include/aow/can_stall.h`, unit-tested): frames queued, none sent
  since the last look, controller error-active with a transmit error count of 0, three looks
  running. Then the interface goes down and up, a warning is logged and `/api/status`
  `canRestarts` counts it; ten seconds' grace follows. A channel alone on its bus (queued,
  none sent, but error-passive at 128) is left alone: CAN1 on the dev unit sits in that state
  and is not touched. `tc` and `ip` are only run for a channel that has sent nothing since the
  last look.
- AgIsoStack++'s SocketCAN driver closes its socket for good when its interface goes down.
  `daemon/isobus.cpp` reopens it. Checked by cycling `hatcan2` by hand with the implement
  connected: reopened within a second, the implement stayed connected.
- **The stuck state itself has not been reproduced**, so the watch has not been seen to catch
  the real thing. CAN2 high shorted to CAN2 low for 14 s gave 90 bus-off restarts, and the
  channel came back by itself as soon as the short was removed; the implement reconnected
  3 s later and the watch rightly did nothing. Two channels at different bit rates under load
  gave errors but no bus-off. The link wired crossed again (high of one bus to low of the
  other, as when it first happened) for 48 s gave 324 bus-off restarts on CAN2 and one on
  CAN3, and again both channels came back by themselves once the link was right; the
  implement reconnected. So bus-off recovery by `restart-ms` works on this hardware in every
  case tried, the one stuck channel is unexplained, and the watch stays as insurance.
- The daemon was warning every minute about ignored datagrams from the unit's own LAN
  address: AgOpenWeb's broadcast copies of what it also sends on loopback (section 5, the
  loopback lock). They are still dropped, but no longer counted as another host's or warned
  about; only a genuinely foreign sender is.

Not done yet: PGN 240 back to the app, upstream's status screen for a tractor's terminal
(decide with AgIsoVT on the bench), a real implement, and two real CAN nodes (two HAT channels
wired together would do).

**Command latency (2026-10-11).** Time from a PGN 254 with new section bits leaving for the
daemon to the setpoint frame (DDI 290) arriving on the other linked HAT channel, 150 changes a run.

- The "42 to 63 ms" above was mostly the test: the probe looks every 20 ms and the simulator
  every 5 ms. Measured on the bus it was 6 to 16.5 ms, median 11 ms.
- Two waits made that up, and both are gone. The control thread passed the bits on at its 10 ms
  tick: it now does so as the PGN arrives (the tick still clears them when the app goes away).
  The ISOBUS thread looked every 10 ms: `Shared::setIsoSections()` now wakes it through an
  eventfd when the bits change, and it sends setpoints before reading reported states.
- After: 1.0 to 2.7 ms, median 1.5 ms, three runs. ISOBUS thread CPU idle is unchanged (under 2 %).
- What is left is the implement's own reaction and the valves, which this cannot see, and the
  app's rate of sending PGN 254.

**Open.**

- Whether the app should learn section count and width from the implement (upstream sends them
  in PGN 240). That is AgOpenWeb work; until then the user enters them in the app as today.
- Whether AgIsoStack++ stays fetched at build time or is vendored: fetching needs the network
  wherever a package is built.
- Rate control: upstream does not do it.

