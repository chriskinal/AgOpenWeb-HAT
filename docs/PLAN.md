# Plan

> Decisions, scope and build order for `aow-hatd`. Written 2026-09-28.
> Source being ported: `Firmware_Teensy_AiO_26` v26.5.5 (Teensy 4.1).
> HAT hardware: AgOpenWeb `Reference/HARDWARE_HAT_DESIGN.md`.

## 1. Decisions

| Decision | Choice | Why |
|---|---|---|
| Base OS | Raspberry Pi OS Lite, 64-bit | best fit with the Pi overlays and bootloader settings the HAT needs |
| Kernel | PREEMPT_RT, self-built | a bounded time to switch the motor off |
| Protocol to the app | AgOpenGPS PGN over UDP, loopback | the app treats the HAT like an AiO board; existing boards keep working |
| Teensy firmware | unchanged | it stays on the AiO boards people own |
| HAT present? | asked once at install | reliable; the HAT has no ID EEPROM |
| Repo | this one, separate from AgOpenWeb | |
| Language | C++17 | the firmware logic ports with few changes |

## 2. What comes from the firmware

Follow the firmware **code**, not its docs: `CLAUDE.md` and `docs/` there have stale pin tables
and rates.

| Firmware module | Here | Notes |
|---|---|---|
| `PGNProcessor`, `PGNUtils` | `core/pgn` (done) | |
| `AutosteerProcessor` | core | 100 Hz loop, proportional controller, soft start, PGN 251/252/254 in, 253 out |
| `KickoutMonitor` | core | current-sense kickout only on the HAT, plus Keya and CAN kickouts |
| `ADProcessor` | core (scaling) + HAT ADC driver | WAS = (counts - 2048 - offset) / countsPerDegree |
| `GNSSProcessor`, `NAVProcessor`, `NMEAMessageBuilder`, parsers | core | GGA/GNS/VTG/HPR/KSXT and Unicore INSPVAA/INSPVAXA in; PANDA/PAOGI out |
| `RTCMProcessor` | core | UDP 2233 in, GNSS UART out |
| `TractorCANDriver`, `CANProtocolEngine`, `CANConfigParser` | core + SocketCAN driver | Keya CAN and the tractor brands; JSON brand files |
| `PWMMotorDriver` | new HAT driver | the HAT is PWM + DIR + EN, not two PWM pins |
| `LEDManagerFSM` | core (states) + SK6812 driver | |
| `ConfigManager` (EEPROM) | JSON file | |
| `UM98xManager` | later | receiver setup |

**Not ported, Linux or AgOpenWeb provides it:** Ethernet stack, DHCP server, web UI, OTA update,
scheduler, logging, the GVRET CAN sniffer (use `candump`), the ESP32 bridge, subnet change.

**No hardware on the HAT:** section and hydraulic outputs, a separate IMU, the steering-wheel
encoder, the pressure sensor, the speed pulse, the Danfoss valve. Sections stay on an external
machine module or go over CAN.

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

| Phase | Work | Done when |
|---|---|---|
| 1 | OS bring-up: RT kernel, overlays, `can0`-`can2`, UARTs, ADC, PWM, watchdog, key-off monitor | each device answers from the shell |
| 2 | GNSS: receiver in, PANDA/PAOGI out, RTCM through | AgOpenWeb shows an RTK fix from the HAT |
| 3 | Steering: ADC, motor driver, PGN 251-254, kickout, timeouts | bench motor follows the app; every stop path tested |
| 4 | CAN: Keya and tractor brands | one brand verified on a machine |
| 5 | LEDs, piezo, settings file, packaging, the AgOpenWeb changes | clean install on a fresh image, both hardware choices |
| 6 | Latency measurement under load | worst case recorded; decide if anything more is needed |

## 7. Open items

- PWM frequency for the motor driver. The firmware uses 75 Hz for the DRV8701; the HAT's external
  drivers (MD13S, IBT-2 class) usually want much higher. To decide in phase 3.
- Which ADC path: the kernel IIO driver or direct `spidev`. To decide in phase 1.
- LED timing from the PCM pin under an RT kernel. To test in phase 5.
- Whether `pinctrl` is the right tool for the service unit's stop action. To verify in phase 1.
