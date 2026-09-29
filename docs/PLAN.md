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
| `RTCMProcessor` | 210 | `core/gnss_link` (done) | UDP 2233 and the radio (RS-232 #1) to the GNSS UART |
| `EventLogger` | 600 | `core/log` + `daemon/web_thread` (done) | same levels, sources and rate limits; console → journald, UDP syslog, ring buffer, WebSocket 8083 |
| `ConfigManager`, `EEPROMLayout` | 1,040 | `core/settings`, `daemon/shared` (done) | `/etc/aow-hat/settings.json`, written atomically; same fields and defaults |
| `SimpleHTTPServer`, `SimpleWebSocket`, `SimpleWebManager`, `LogWebSocket`, `TelemetryWebSocket` | 3,000 | `web/server`, `web/api` (done for Home, Device, Event logger, Logs, Network) | same routes; ports 80, 8082, 8083 so the pages work unchanged |
| `web_pages/*` (the served set, ~168 KB) | | `web/pages/` (6 of 11 so far) | copied as-is; HAT changes marked in each file |
| `AutosteerProcessor` | 1,650 | `core/steer` (done, except CAN valve gating: phase 5) | 100 Hz, P controller, min/high PWM, soft start, Ackermann, engage by switch or button, valve-ready gating |
| `KickoutMonitor` | 700 | `core/steer` (current done; CAN and Keya: phase 5) | current-sense, CAN and Keya kickouts; PGN 250 from current |
| `ADProcessor` | 840 | `core/steer` (done) + HAT ADC driver | WAS and current as the firmware; work switch digital only; adds remote, supply and key sense |
| `PWMMotorDriver` | 340 | HAT driver | rewritten for PWM + DIR + EN; brake mode only where the driver supports it |
| `MotorDriverManager`, `KeyaSerialDriver` | 590 | core + RS-232 #2 | Keya over serial, `E2` probe |
| `TractorCANDriver`, `CANProtocolEngine`, `CANConfigParser` | 2,460 | core + SocketCAN | all 9 brands, JSON brand files, Keya CAN, CAN wheel angle, CAN engage buttons, CAN kickout |
| `GVRETServer` | 390 | daemon | SavvyCAN on TCP 23 over SocketCAN |
| `MessageBuilder` | 140 | core | PGN 221 popups |
| `LEDManagerFSM` | 650 | core (states) + SK6812 driver | PWR, GPS, STEER, INS on the 4 LEDs |
| `HardwareManager` buzzer | | piezo driver | same sounds and volume setting |
| `UM98xManager` | 645 | core + GPS page | read/write receiver CONFIG, MODE, logs |
| `CommandHandler` | 300 | `aow-hatctl` (later) | the console keys as CLI commands over a local socket |
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
  accepts only PANDA and PAOGI, so with it on AgOpenWeb loses GPS. The page says so.
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
| 5 | CAN: SocketCAN, JSON engine, the 9 brands, Keya CAN and serial, CAN page and upload, GVRET | bench tests only | one brand verified on a machine |
| 6 | UM98x page, radio RTCM on RS-232 #1 | receiver | receiver config read and written from the page |
| 7 | LEDs, piezo, `aow-hatctl`, update page, packaging, the AgOpenWeb changes | yes | clean install on a fresh image, both hardware choices |
| 8 | Latency measurement under load | yes | worst case recorded; decide if anything more is needed |

### Phase 1 record

| Date | Check | Result |
|---|---|---|
| 2026-09-28 | RT kernel on a CM4 Rev 1.1 (2 GB), Trixie Lite, on Wi-Fi | `6.18.50+rpt-rpi-v8-rt` boots; `config.txt.pre-rt` kept as the fallback |
| 2026-09-28 | `cyclictest -m -S -p90 -i1000 -D60`, idle | max 42 µs, avg 9-10 µs |
| 2026-09-28 | `aow-hatd` at FIFO 80, `pgn_probe.py` 2000 x PGN 254 at 100 Hz | 2000/2000 answered, round trip median 0.18 ms, worst 0.35 ms |
| 2026-09-28 | `gnss_probe.py` (pty as the receiver) | GGA -> PANDA 20/20, HPR -> PAOGI, RTCM 2233 -> serial byte-identical |
| 2026-09-28 | `steer_probe.py`, service with `--board sim` | armed by guidance on, motor within 100 ms, wheel at 5 deg in 0.81 s and holding; 2 s timeout logged; PGN 252 gains saved |
| 2026-09-28 | `install.sh --hat` on the dev CM4, `web_probe.py` on port 80 | control thread FIFO 80, web thread normal; all pages and APIs; telemetry 101 Hz; log WebSocket; settings saved atomically to `/etc/aow-hat/settings.json`; Restart button -> systemd restart; journal priorities work |

## 7. Open items

- Current sense scale. `kCurrentCountsPerVolt` in `core/steer.h` maps HAT volts to the
  firmware's Teensy counts so the app's current threshold keeps its meaning; set it from the
  motor driver's current output when one is on the bench.
- The steer timeout stays at the firmware's 2 s. AgOpenWeb sends PGN 254 at 100 Hz, so a
  shorter value is possible, but the app has known UI stalls of 1-3 s (its own comments); decide
  with the bench motor, which would stop mid-turn on every stall.

- The web thread's Home page links to CAN, GPS and System Update, which return 404 until
  phases 5-7 add them.

- PWM frequency for the motor driver. The firmware uses 75 Hz for the DRV8701; the HAT's external
  drivers (MD13S, IBT-2 class) usually want much higher. To decide in phase 3.
- Which ADC path: the kernel IIO driver or direct `spidev`. To decide in phase 1.
- LED timing from the PCM pin under an RT kernel. To test in phase 5.
- Whether `pinctrl` is the right tool for the service unit's stop action. To verify in phase 1.
- INSPVAXA field numbers. The port keeps the firmware's (velocities at 15-17), but Unicore's
  layout has undulation after height, which would put them at 16-18 and make the firmware's speed
  wrong. Settle with a real UM981 capture.
- GNSS serial device on the HAT (UART5's `/dev/ttyAMA*` name) and the `--gnss` line in the
  service unit. Until then, `aow-hatd --gnss /dev/ttyUSB0` runs a USB receiver.
