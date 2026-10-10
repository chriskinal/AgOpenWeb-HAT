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
second GPS UART. Sections stay on an external machine module or go over CAN.

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
| RTCM forward target configurable (now `192.168.5.255:2233`) | `MainViewModel.Ntrip.cs:110-111` | AiO boards on 192.168.5.x |
| Lock to a loopback module, so 100 Hz packets stop being broadcast on every interface | `UdpCommunicationService` `LockToSubnet` | discovery of LAN modules |
| Service unit: start after `aow-hatd` when a HAT is fitted | `deploy/linux/agopenweb.service` | installs with no HAT |

## 6. Build order

Hardware-free work goes first, while the board is being made.

| Phase | Work | Needs the HAT | Done when |
|---|---|---|---|
| 1 | OS bring-up: RT kernel (done), overlays, `can0`-`can2`, UARTs, ADC, PWM, watchdog, key-off monitor | yes | each device answers from the shell |
| 2 | GNSS: receiver in, PANDA/PAOGI out, RTCM through (done, except the HAT UART) | UART only | AgOpenWeb shows an RTK fix from the HAT |
| 3 | Foundation: event logger, settings file, web server with Home, Device, Event logger and Logs pages, telemetry (done) | no | pages work on the dev CM4 |
| 4 | Steering in core: PGN 251/252, controller, soft start, kickouts, timeouts, PGN 250 (done; `--board sim` closes the loop on a bare CM4); then the HAT drivers | drivers only | every stop path unit-tested (done, mutation-checked); bench motor follows the app |
| 5 | CAN: SocketCAN, JSON engine, the 9 brands, Keya CAN and serial, CAN page and upload, GVRET (done on vcan) | bench tests only | one brand verified on a machine |
| 6 | UM98x page, radio RTCM on RS-232 #1 (done, on ptys) | receiver | receiver config read and written from the page |
| 7 | LED and buzzer logic, `aow-hatctl`, installer (done); update page, LED and piezo drivers, the AgOpenWeb changes | yes | clean install on a fresh image, both hardware choices |
| 8 | Latency measurement under load | yes | worst case recorded; decide if anything more is needed |

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
| 2026-10-10 | PWM0 at 100 Hz, 50 %, metered at J3.10 | 1.647 V on the Fluke (3.3 V x 0.5); J3.11 DIR and J3.12 EN at 0 V from boot |
| 2026-09-28 | `install.sh --hat` on the dev CM4, `web_probe.py` on port 80 | control thread FIFO 80, web thread normal; all pages and APIs; telemetry 101 Hz; log WebSocket; settings saved atomically to `/etc/aow-hat/settings.json`; Restart button -> systemd restart; journal priorities work |

## 7. Open items

**Paused 2026-09-29, waiting for the HAT.** Everything that needs no hardware is done and on
`main`. Resume with, in order:

1. ~~Phase 1 bring-up on the board~~ **done 2026-10-10** (phase 1 record above). Still to
   done from the shell as well: the watchdog, RS-232 #1 loopback, the key monitor as a
   service, both RS-232 ports looped (record above).
2. ~~The HAT board in `boards/hat`~~ **done 2026-10-10** for ADC, switches, PWM + DIR + EN and
   the watchdog; `--board hat` and the service unit's `--gnss`, `--radio`, `--keya-serial`,
   `--can`, `--adc-ref` lines are in. Still to write: SK6812 LEDs (GPIO21, PCM) and the piezo
   (GPIO23, software PWM), both stubs in `HatBoard`. Not yet bench-tested with a WAS pot and
   a meter on the motor outputs.
3. Bench: motor follows the app (`steer_probe.py` against real hardware), every stop path, the
   PWM frequency, `kCurrentCountsPerVolt`, the steer timeout, CAN bit rate and bus-off recovery,
   one INSPVAXA capture from a UM981.
4. Decide the System Update page: the firmware's is unauthenticated; recommended is signed
   packages only (or none: update by `install.sh` over SSH).
5. The AgOpenWeb changes (section 5), in that repo.

The dev CM4 (`chris@192.168.4.217`) runs `aow-hatd` as a service with `--board sim` and default
settings. Its `~/AgOpenWeb-HAT` is an rsync copy, not a git checkout.

- CAN bit rate is set with `ip link` at startup, as root. Check on the HAT's MCP251863s
  (`mcp251xfd`) that `restart-ms 100` brings a bus back after bus-off.
- The brand JSON files are in `deploy/can-config/`; the upload page takes a file from the
  user's device. Decide whether the installer puts them somewhere the page can offer directly.

- Current sense scale. `kCurrentCountsPerVolt` in `core/steer.h` maps HAT volts to the
  firmware's Teensy counts so the app's current threshold keeps its meaning; set it from the
  motor driver's current output when one is on the bench.
- The steer timeout stays at the firmware's 2 s. AgOpenWeb sends PGN 254 at 100 Hz, so a
  shorter value is possible, but the app has known UI stalls of 1-3 s (its own comments); decide
  with the bench motor, which would stop mid-turn on every stall.

- The Home page's System Update button returns 404: the update page is not built yet (see the
  decision below).

- PWM frequency for the motor driver. The firmware uses 75 Hz for the DRV8701; the HAT's external
  drivers (MD13S, IBT-2 class) usually want much higher. To decide in phase 3.
- ~~Which ADC path: the kernel IIO driver or direct `spidev`.~~ **`spidev`** (decided 2026-10-10):
  the Pi kernel has no `ti-adc128s052` module, and one 18-byte transfer reads all channels.
- LED timing from the PCM pin under an RT kernel. To test in phase 5.
- Whether `pinctrl` is the right tool for the service unit's stop action. To verify in phase 1.
- INSPVAXA field numbers. The port keeps the firmware's (velocities at 15-17), but Unicore's
  layout has undulation after height, which would put them at 16-18 and make the firmware's speed
  wrong. Settle with a real UM981 capture.
- ~~GNSS serial device on the HAT~~ `/dev/ttyAMA5` (2026-10-10); RS-232 #1 is `/dev/ttyAMA2`,
  RS-232 #2 `/dev/ttyAMA3`. The service unit still needs the `--gnss` line.
