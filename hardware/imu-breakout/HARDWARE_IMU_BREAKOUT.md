<!-- SPDX-License-Identifier: CERN-OHL-P-2.0 -->
<!-- Copyright (c) 2026 The AgOpenWeb-HAT contributors -->

# IMU Breakout — ST LSM6DSK320X

> A small breakout board for evaluating the **ST LSM6DSK320X** 6-axis IMU (gyro + low-g and high-g
> accelerometers, "enhanced vibration immunity") as a candidate roll/heading sensor for AgOpenWeb.
> It mounts rigidly with two M3 screws and plugs into a Raspberry Pi 40-pin header (CM4 IO Board or
> any Pi) over I²C or SPI at 3.3 V.
>
> **Status (2026-11-03): parked; the sample is on its way.** Designed on paper, not drawn. Two
> assembled boards from JLC come to about $125; ST's own DIL24 adapter for this part,
> **STEVAL-MKI257A**, is the usual $15–30 and carries the same circuit (sensor, 100 nF, 10 µF). A
> sample is due 2026-11-05 and is evaluated on the **STEVAL-MKI109V3** motherboard (section 8), so
> no Pi and no breakout are needed for the test. Kept as the reference if the adapter is not what
> arrives or a bolt-down board is needed (the DIL24 module has no mounting holes and no pull-ups: a
> Pi's I²C1 has its own). This file is the complete document set for the board: design, netlist,
> BOM and open items. Designed for **EasyEDA Standard**, 2 layers.
>
> **Sources:**
> - ST datasheet **DS15060 Rev 1 (July 2026)**, `DS_lsm6dsk320x.pdf`, 210 pages: electrical limits,
>   interfaces, register map, soldering. The data brief DB5732 (Jan 2026) agrees with it on pinout and
>   package. The sister part LSM6DSV320X (DS14623) has the identical package, pinout and pin table.
> - ST's adapter board for the sister part, STEVAL-MKI251A (DB5503): the decoupling scheme.
>   ST's adapter for this part, STEVAL-MKI257A, is announced for November 2026.

---

## 1. Why this board

The AiO boards use a BNO085; the EMAX UM981 has its own IMU. Both suffer on a running tractor
from engine and implement vibration. ST's pitch for the LSM6DSK320X is a new gyroscope design
with "enhanced vibration immunity". The question this board answers: **is the roll/heading output
cleaner than the BNO085 under real vibration?** Everything else (high-g channel, machine learning
core, OIS/EIS channels) is not needed and is not brought out beyond what costs nothing.

Design rules, as for the HAT: field-proven simplicity, no over-engineering. No regulator, no level
shifter, nothing the Pi doesn't already supply.

## 2. The part in one table

| | LSM6DSK320X | Notes |
|---|---|---|
| Package | LGA-14L, 2.5 × 3.0 × 0.83 mm, 0.5 mm pitch | pads 0.25 × 0.475 mm (DS15060 §17) |
| VDD | 1.71 – 3.6 V (typ 1.8) | DS15060 Table 4 |
| VDDIO | 1.08 – 3.6 V (I²C needs ≥ 1.62 V) | DS15060 Table 4, p.1 |
| Abs. max on any pin | 4.8 V supply; control pins −0.3 to VDDIO + 0.3 | DS15060 Table 9; **not 5 V tolerant** |
| Current | 0.67 mA, 6 axes, high-performance | negligible on the Pi's 3.3 V |
| Interfaces | I²C fast mode and fast mode plus (400 kHz / 1 MHz), SPI 3/4-wire to 10 MHz, MIPI I3C | DS15060 Tables 6–8; SPI mode 0 or 3 |
| I²C address | `0x6A` (SA0 low), `0x6B` (SA0 high) | DS15060 §5.1.2 |
| WHO_AM_I (0x0F) | **`0x75`** | DS15060 §9.13 |
| INT1, INT2 | programmable outputs, push-pull by default | don't pull them |
| Gyro turn-on | 40 ms | DS15060 Table 4 |
| Temperature | −40 … +85 °C | |
| Ordering | LSM6DSK320XTR, tape and reel | JLCPCB **C53025794**, Extended, 81 in stock on 2026-10-06 (K2) |

**Pin table (DS15060 Table 2, mode 1 = plain I²C/SPI, which is all this board uses by default):**

| Pin | Name | Mode 1 | On this board |
|---|---|---|---|
| 1 | SDO/TA0 | SPI MISO; I²C address LSB | J2.1, R4 to SJ3 (VDDIO default) |
| 2 | SDx | "connect to VDDIO or GND" | J3.1, SJ4 to GND (closed) |
| 3 | SCx | "connect to VDDIO or GND" | J3.2, SJ5 to GND (closed) |
| 4 | INT1 | interrupt 1 | J2.3 |
| 5 | VDDIO | I/O supply, 100 nF | C2, SJ1 to VDD (closed), J1.3 |
| 6 | GND | | GND |
| 7 | GND | | GND |
| 8 | VDD | supply, 100 nF | C1, C3, J1.2 |
| 9 | INT2 | interrupt 2 | J2.4 |
| 10 | OCS_aux | "connect to VDDIO or leave unconnected" | J3.3, otherwise open |
| 11 | SDO_aux | "connect to VDDIO or leave unconnected" | J3.4, otherwise open |
| 12 | CS | 1 = I²C/I3C, 0 = SPI | J2.2, R3 10 k to VDDIO |
| 13 | SCL | I²C SCL / SPI SCK | J1.4, R1 10 k via SJ2 |
| 14 | SDA | I²C SDA / SPI MOSI (SDI) | J1.5, R2 10 k via SJ2 |

Pins 10 and 11 "unconnected" means soldered to an isolated pad (Table 2 note 2), never floating in
the air.

**Pin 1 orientation (DS15060 Fig. 5, bottom view):** pins 12-13-14 along the top edge (CS, SCL,
SDA, left to right), 1-2-3-4 down the right edge (SDO on top), 5-6-7 along the bottom (VDDIO at the
right), 8-9-10-11 up the left edge (VDD at the bottom). Seen from the top, pin 1 is therefore at the
**top-left** corner, marked by the dot. Axes (top view): X to the right, Y away, Z up.

## 3. Circuit

```
J1.2 VDD ──┬── C1 100 nF ── GND          (at pin 8)
           ├── C3 10 µF  ── GND
           └── SJ1 (closed) ── VDDIO ──┬── C2 100 nF ── GND   (at pin 5)
                                        ├── J1.3
                                        ├── R3 10 k ── CS (pin 12) ── J2.2
                                        ├── SJ2 (closed) ──┬── R1 10 k ── SCL (13) ── J1.4
                                        │                  └── R2 10 k ── SDA (14) ── J1.5
                                        └── SJ3 pad A ─┐
                               GND ──── SJ3 pad B ─┴── SJ3 centre ── R4 10 k ── SDO/SA0 (1) ── J2.1
SDx (2) ── SJ4 (closed) ── GND ;  SDx ── J3.1
SCx (3) ── SJ5 (closed) ── GND ;  SCx ── J3.2
OCS_aux (10) ── J3.3 (pad only) ;  SDO_aux (11) ── J3.4 (pad only)
INT1 (4) ── J2.3 ;  INT2 (9) ── J2.4 ;  GND ── J1.1, J2.5, pins 6, 7
```

**Why each piece:**
- **C1, C2, C3.** The datasheet asks for 100 nF on VDD and on VDDIO. ST's adapter board adds 10 µF
  on VDD. Same here. Put C1 and C2 within 2 mm of pins 8 and 5.
- **SJ1 VDD–VDDIO, closed.** One 3.3 V rail by default. Open it and feed J1.3 to test the sensor with
  a 1.8 V I/O supply (SPI timing is specified at VDDIO 1.8 V and 1.2 V, Tables 6 and 7); every
  pull-up on the board hangs off VDDIO, so they follow.
- **R1, R2 through SJ2, closed.** 10 k I²C pull-ups to VDDIO. A Pi already has 1.8 k on I²C1, so
  with a Pi they're parallel and harmless; they matter on hosts without pull-ups. Open SJ2 for SPI if
  the extra load is unwanted (it isn't at 10 MHz with 10 k, so default is closed).
- **R3 on CS.** 10 k to VDDIO: the part wakes in I²C mode with nothing connected to CS. An SPI host
  drives CS and the resistor is irrelevant.
- **R4 + SJ3 on SDO/SA0.** In I²C mode this pin is the address LSB; in SPI it is MISO, an **output**,
  so it must never be tied hard to a rail. R4 (10 k) sets the address through SJ3: bridge A for
  `0x6B` (default), B for `0x6A`. In SPI mode the 10 k is only a weak pull on MISO.
- **SJ4, SJ5 on SDx, SCx, closed to GND.** Mode 1 requires them tied to VDDIO or GND. GND is
  chosen so that opening the jumper and driving J2 cannot short a supply. Open them to try mode 2
  (I²C controller for an external sensor) or mode 3 (auxiliary SPI/I3C).
- **J2 pads only.** OCS_aux and SDO_aux are allowed to be unconnected in mode 1. Pads give access for
  mode 3 experiments without putting anything on the net by default.
- **No series resistors, no ESD parts, no regulator.** 3.3 V in, 3.3 V logic, short leads to a Pi.
  The pins are not 5 V tolerant: say so on the silkscreen ("3V3 ONLY").

## 4. Connectors

**J1 and J2: two 1 × 5 2.54 mm male headers on opposite edges, rows 15.24 mm (0.6") apart.** The
board is then a 10-pin DIP-style module: it plugs into a breadboard or a pair of female headers and
is held flat at both edges, instead of hinging on one row. Everything a plain I²C hookup needs is on
J1; SPI adds J2.1 and J2.2. Both rows number left to right seen from the top, with the labels on the
silkscreen.

| J1 (bottom) | Net | | J2 (top) | Net |
|---|---|---|---|---|
| 1 | GND | | 1 | `SDO` (SA0 / MISO) |
| 2 | `VDD` (3.3 V in) | | 2 | `CS` |
| 3 | `VDDIO` | | 3 | `INT1` |
| 4 | `SCL` (SCK) | | 4 | `INT2` |
| 5 | `SDA` (MOSI / SDI) | | 5 | GND |

**J3, 1 × 4, 2.54 mm pads, header not fitted, inboard:** 1 `SDx`, 2 `SCx`, 3 `OCS_aux`,
4 `SDO_aux`. Left as pads so the aux pins can't be bumped on a breadboard.

**Hookup to a Pi 40-pin header (CM4 IO Board without the HAT):**

| Breakout | I²C (I²C1) | SPI (SPI0) |
|---|---|---|
| J1.2 VDD | pin 1 (3V3) | pin 1 |
| J1.1 GND | pin 6 | pin 6 |
| J1.4 SCL | pin 5 (GPIO3, SCL1) | pin 23 (GPIO11, SCLK) |
| J1.5 SDA | pin 3 (GPIO2, SDA1) | pin 19 (GPIO10, MOSI) |
| J2.1 SDO | — | pin 21 (GPIO9, MISO) |
| J2.2 CS | — | pin 24 (GPIO8, CE0) |
| J2.3 INT1 | any free GPIO (optional) | same |

## 5. Mechanical and layout

**Board: 26 × 22 mm, 2-layer, 1.6 mm FR-4, ENIG** (the 0.25 mm pads at 0.5 mm pitch want a flat
finish; HASL works but is harder to hand-reflow). Coordinates below: origin at the top-left corner,
x right, y down, in mm, as in the HAT docs.

| Item | Position | Notes |
|---|---|---|
| J2 | pins at x 7.92 … 18.08, y 3.38 | 1 × 5 along the top edge, pin 1 at the left |
| J1 | pins at x 7.92 … 18.08, y 18.62 | 1 × 5 along the bottom edge, pin 1 at the left; 15.24 mm below J2 |
| H1, H2 | (3.5, 11), (22.5, 11) | Ø 3.2 unplated, M3, on the centreline, 19 mm apart. Keep copper 3.5 mm from each centre |
| U1 | centred at (13, 11) | X axis along board x, pin 1 at the top-left; ≥ 3 mm from holes, headers and edges |
| C1, C3 | left of U1 (VDD, pin 8) | C1 nearest the pin; between U1 and H1 |
| C2 | below U1 (VDDIO, pin 5) | |
| R1–R4, SJ1–SJ5 | right of U1, between U1 and H2 | SJ3 is the 3-pad one; label A = 0x6B, B = 0x6A |
| J3 | pads at x 9.2 … 16.8, y 7.0 | 1 × 4 inboard between J2 and U1, pads only |

**Rules for the sensor:**
- **Land pattern 1 : 1 with the package pads:** 14 × 0.25 × 0.475 mm on a 0.5 mm pitch, 3 pads per
  short side and 4 per long side exactly as the package drawing (DS15060 §17); solder mask opening 0.05 mm larger
  per side. ST's TN0018 is the reference for LGA MEMS land patterns and stencils (K4).
- **Nothing under U1:** no vias, no tracks, and keep the ground pour 0.5 mm clear of the package
  outline on the top layer. MEMS parts read board stress as offset.
- **Keep U1 away from the screws and the edge** (hence the 3 mm rule above) for the same reason.
- Ground pour on both layers, stitched; 0.2 mm tracks and clearance, 0.3 mm for VDD/GND.
- Silkscreen: pin-1 dot, **X / Y arrows and a Z dot matching DS15060 Fig. 5**, "3V3 ONLY", J1 pin
  names, SJ3 "A=6B B=6A", board name and date.

**Mounting.** On the bench the two header rows carry the board: a breadboard, or two 1 × 5 female
headers on a carrier, hold it flat with no screws. For the vibration test use the two M3 screws onto
the same bracket as the reference IMU (BNO085 board or the AiO), board X along the tractor's forward
axis, Z up, leads short and tied down. A loose breakout on jumper wires measures the wires, not the
tractor.

**EasyEDA footprint.** Place the part as LCSC **C53025794** (LSM6DSK320XTR, `LGA-14L(2.5x3)`).
If the library has no symbol or footprint for it yet, take them from **LSM6DSVTR (C5267399)**,
identical package and pin order, and set the LCSC attribute to C53025794 so JLC fits the right
part. Either way check pin numbering against §2 before placing (B1).

## 6. Netlist

Pin notation as in the HAT netlist: `R1.1` = R1 pin 1; `SJ3.A`/`SJ3.B`/`SJ3.C` for the 3-pad jumper.

| Ref | Part |
|---|---|
| U1 | LSM6DSK320XTR |
| C1, C2 | 100 nF 0402 |
| C3 | 10 µF 0603 |
| R1–R4 | 10 kΩ 0402 |
| J1, J2 | 1 × 5 pin header, 2.54 mm |
| J3 | 1 × 4 pads, 2.54 mm |
| SJ1, SJ2, SJ4, SJ5 | 2-pad solder jumper (footprint only) |
| SJ3 | 3-pad solder jumper (footprint only) |
| H1, H2 | Ø 3.2 unplated hole |

| Net | Pins |
|---|---|
| `VDD` | U1.8, C1.1, C3.1, SJ1.1, J1.2 |
| `VDDIO` | U1.5, C2.1, SJ1.2, J1.3, R3.1, SJ2.1, SJ3.A |
| GND | U1.6, U1.7, C1.2, C2.2, C3.2, J1.1, J2.5, SJ3.B, SJ4.2, SJ5.2 |
| `PU` | SJ2.2, R1.1, R2.1 |
| `SCL` | U1.13, R1.2, J1.4 |
| `SDA` | U1.14, R2.2, J1.5 |
| `SDO` | U1.1, R4.2, J2.1 |
| `SA0_SEL` | SJ3.C, R4.1 |
| `CS` | U1.12, R3.2, J2.2 |
| `INT1` | U1.4, J2.3 |
| `INT2` | U1.9, J2.4 |
| `SDX` | U1.2, SJ4.1, J3.1 |
| `SCX` | U1.3, SJ5.1, J3.2 |
| `OCS_AUX` | U1.10, J3.3 |
| `SDO_AUX` | U1.11, J3.4 |

Defaults: SJ1, SJ2, SJ4, SJ5 **closed**; SJ3 **A–C bridged** (address `0x6B`).

## 7. BOM

| Ref | Qty | Part | Package | LCSC | Type | Notes |
|---|---|---|---|---|---|---|
| U1 | 1 | LSM6DSK320XTR | LGA-14L 2.5 × 3 | C53025794 | Ext | 81 in stock, ~$8.45 each at 1–9, 2026-10-06; let JLC fit it |
| C1, C2 | 2 | 100 nF 16 V X7R | 0402 | C1525 | Basic | as on the HAT |
| C3 | 1 | 10 µF 25 V X5R | 0603 | C96446 | Basic | as on the HAT |
| R1–R4 | 4 | 10 kΩ | 0402 | C25744 | Basic | as on the HAT |
| J1, J2 | 2 | 1 × 5 pin header 2.54 mm | TH | e.g. C2337 (1 × 40, snap off) | Basic | check number at order time |
| J3 | — | pads only | | — | | |
| SJ1–SJ5 | — | solder jumpers, footprint only | | — | | |
| H1, H2 | — | Ø 3.2 unplated | | — | | |

## 8. Evaluation plan

### 8.1 What is being measured

The IMU's job on the HAT is roll, and heading between GPS fixes. AgOpenWeb moves the antenna
position by the roll, so one degree of roll error is about 5 cm at a 3 m antenna. The datasheet
claims "enhanced vibration immunity" but **gives no number for it**: DS15060 has no g-sensitivity
and no vibration rectification spec. The test measures that claim directly, against the IMU the
app uses today (a TM171, or a BNO085 in RVC mode), on the same plate, at the same measured
vibration. Two questions: does roll stay true and quiet while the engine and implement shake it,
and does the gyro's zero shift?

What the datasheet does give, for the still baseline (DS15060 Table 4, typical):

| | Value | What it means |
|---|---|---|
| Gyro rate noise density | 3.8 mdps/√Hz, any ODR and FS | angular random walk ≈ 0.23 °/√h; 42 mdps RMS at 240 Hz ODR (BW = ODR/2) |
| Gyro zero-rate level | ±1 dps after calibration; ±0.006 dps/°C | |
| Accel noise density (low-g) | 60 µg/√Hz | 0.66 mg RMS at 240 Hz: 0.04° of tilt |
| Accel zero-g offset | ±12 mg; ±0.07 mg/°C | 0.7° of tilt if uncalibrated: calibrate, or use the fusion output |
| On-chip fusion (SFLP) roll | 0.5° low dynamics, 1.2° high dynamics (Table 1) | a gaming spec; log raw data too, so a filter in the daemon can be judged against it |

ST's design tips in `imu/` (not committed): **DT0064** is the Allan deviation method used for the
still baseline; **DT0106**'s residual linear acceleration (acceleration minus the rotated gravity
vector) is the measure of how hard the sensor is being shaken, so the two IMUs are compared at the
same dose. The SFLP block outputs the gravity vector and the gyro bias on-chip, so that subtraction
comes free.

### 8.2 The rig: STEVAL-MKI109V3, no Pi

The sample is the STEVAL-MKI257A DIL24 adapter, and it plugs into the **STEVAL-MKI109V3**
motherboard (on hand). ST's desktop tool on the Mac, **MEMS Studio** (or the older Unico-GUI),
reads the part over USB: raw accel and gyro at the full ODR, the FIFO with timestamps, the
embedded-function registers for the SFLP output, and a register panel for anything else, all
logged to a text file. That is the whole logger. The Pi hookup in §4 and the breakout stay as the
fallback.

- **The tool must know the part.** The LSM6DSK320X is a July 2026 datasheet: use a current MEMS
  Studio and let it update the board's firmware. If the part is still missing from its list, select
  the sister **LSM6DSV320X** profile: same package, pinout and, for the data registers and the
  SFLP block, the same register map, so raw logging works under the wrong name.
- **Mounting.** The sensor sits on the motherboard, so bolt the MKI109V3 **flat to an aluminium
  plate through all its holes** on short standoffs. A board held at one end resonates and the log
  shows the board, not the shaker. The reference IMU bolts beside it, X forward, Z up, leads short
  and tied down.
- **The reference IMU** is logged separately on the Mac: the TM171 on USB by
  `hardware/tools/imu-eval/field_log.py` (Mac, Linux or Windows; Enter bumps a marker column at
  each still/shaking change; a port that drops is reopened). A sharp tap on the plate at the start and end of each run lines the
  two logs up: the TM171's own timestamp runs about 0.27 % fast and wraps every 71.6 min. The
  TM171's gyro output is filtered and often misses the taps (7 dps peak on the sander).
- **The analysis script** is `hardware/tools/imu-eval/analyze.py` (pure Python): it aligns the logs
  on the taps or, when they are not in both, on the shaking (the accel spread matched in each half
  of the run, 0.99–1.00 on every run so far), splits the run at the markers or at motion and at
  steps in its level (idle to working revs), and gives each IMU's roll and pitch per
  segment; with `--still`, drift in 10-minute blocks, against temperature, and the Allan deviation
  (DT0064).
- **The receiver** is logged by the same script with `--um982` (or `--udp 9999`, an AiO's PAOGI),
  to its own file beside the TM171's, with the same clock and markers: HPR, KSXT or PAOGI (heading
  and roll; KSXT's heading quality from its field 11, apart from the position's) with speed and
  fix, from a serial port (port and rate found by itself) or UDP; every sentence also to a `.nmea`
  file.
- **The offline filter** is `hardware/tools/imu-eval/fusion_offline.py`: tilt filters replayed on a
  fusion-off log, for the daemon's own fusion (§8.5).
- **MEMS Studio's quirks.** Turning fusion on turns the high-g channel and the temperature column
  off, and can change the ranges: set them again after. Its "roll" is our pitch (nose up positive)
  and its "pitch" is minus our roll; the quaternion is logged to 3 decimals (0.1°), so tilt is taken
  from the gravity vector, which has the accel's axes and signs.

### 8.3 The tests, in order

1. **Alive (day 1).** WHO_AM_I reads `0x75`. Run the built-in self-test: DS15060 Table 4 gives
   the expected output change (gyro 20–80 dps at ±250 dps; accel 50–1700 mg). Tilt the plate and
   check the axes against the silkscreen.
2. **Still baseline (overnight, B6).** Two hours at rest at 240 Hz, temperature logged, then the
   Allan deviation. Expect the 0.23 °/√h random walk; the curve's floor gives the bias instability
   and the useful averaging time. The reference IMU gets its heading drift at rest over the same
   run.
3. **Bench vibration, the immunity test (B7).** The plate stays at one angle while it vibrates, so
   any tilt change is error (rocking it by hand tilts it for real and tests nothing). Fix the plate
   rigidly to the **body** of a random-orbit sander (hose clamps, no foam), not to the pad: the pad
   orbits at about 200 Hz and well over 100 g, the body gives 0.3–0.8 g, a cab-mount level, set by
   the speed dial. The sander must hold its speed: the first one was failing, ran at 73–83 Hz
   (4400–5000 orbits/min against the usual 8000–12000) and wandered on its own. A random-orbit one
   with a speed dial gives the levels; a fixed-orbit (quarter-sheet palm) sander gives steadier,
   single-frequency shaking, usually at one speed. Too harsh: a thin rubber pad under the sander's
   base, never between the plate and the body. Sander clamped by its handle, pad free and bare, USB leads tied to the body with
   a loose loop to the Mac, the MKI257A tied into its socket. A laptop cooler gives the mild end.
   Sequence: tap, 60 s still, 120 s shaking, 60 s still, tap, at two or three intensities, in two
   orientations (Z up, then on its side, Y up) so the shaking hits different axes; not X up, where
   pitch is 90° and roll undefined. Field checklist: `hardware/tools/imu-eval/vibration_test_checklist.html`. Each intensity twice: fusion
   off with high-g on (low-g ±16 g and gyro ±500 dps at 240 Hz, high-g ±32 g at 480 Hz: the raw
   data for §8.5), then fusion on (the SFLP's answer).
4. **Tractor, same plate, where the HAT will live (B5).** Engine off, idle and PTO speed for five
   minutes each at rest; then an 8 ft rotary cutter (bush hog) on, parked with the PTO running, for
   the implement's own shaking (blades, gearbox, the driveline's joints at twice PTO speed); then
   mowing: a straight pass, a headland turn, the same pass back, a side slope, a rough patch. At
   rest the true roll is constant, so the shift when the engine or the PTO starts is the error with
   no reference needed. For the driving runs the dual-antenna receiver's roll (antennas side by
   side, so its pitch is the tractor's roll) is the vibration-free reference, logged on the laptop
   by `hardware/tools/imu-eval/field_log.py --um982` beside the TM171 (it is not in the daemon's
   `/api/status` yet, K7); its speed and heading are what GNSS aiding (§8.5) needs.

For each run, per IMU:

| Measure | Pass for use on the HAT |
|---|---|
| Roll mean shift, still to shaking | ≤ 0.2° (1 cm at 3 m) |
| Roll standard deviation in 1 s windows while shaking | ≤ 0.3° |
| Gyro mean shift, still to shaking | ≤ 0.1 °/s |
| Vibration dose (residual linear acceleration RMS, DT0106) | logged, so both IMUs are compared at the same level |

### 8.4 Results so far

The MKI109V3 and the TM171 on one aluminium plate, both on USB to the Mac.

- **Test 1, alive (2026-11-06): pass.** WHO_AM_I `0x75`. Self-test (CTRL10 0x19): accel
  +378 / +328 / +241 mg (50–1700), gyro 48.4 / 46.1 / 53.9 dps at ±250 (20–80). Axes as the
  silkscreen: X forward, Y left, Z up; pin-1 dot at the rear left. The TM171 has the same axes, its
  accel the opposite sign (it reports gravity), its gyro in rad/s.
- **Fused tilt at rest, 47 s (SFLP gravity vector against the TM171's own fusion):** roll sd
  0.004° / 0.012°, pitch sd 0.002° / 0.011°. The SFLP's gyro bias matched the raw gyro to 10 mdps;
  its yaw drifts 0.2°/min (no magnetometer), which steering does not use.
- **Rocked by hand, ±45° at up to 300 dps:** the two IMUs agree to 1° rms (mostly the TM171's 20 ms
  sample spacing) and both settle back within 0.1° in a few seconds. Not a vibration test: the plate
  really tilted.
- **Test 2, still baseline, 2 h (2026-11-06): pass (B6).** 240 Hz, fusion off, 24.3–25.6 °C at the
  die.

  | | LSM6DSK320X | TM171 |
  |---|---|---|
  | Gyro random walk X/Y/Z | 0.14 / 0.14 / 0.13 °/√h (2.4 mdps/√Hz) | 0.70 / 0.92 / 0.41 °/√h |
  | Gyro bias instability | 2.0 / 3.1 / 1.5 °/h at 35–140 s | 1.0 / 0.8 / 0.4 °/h at 650 s |
  | Accel noise X/Y/Z | 35 / 36 / 55 µg/√Hz | about 145 µg/√Hz |
  | Accel bias instability | 0.02–0.04 mg | 0.03–0.10 mg |
  | Tilt spread over 2 h, 10-min means (roll / pitch) | 0.011° / 0.009° (from accel) | 0.027° / 0.013° (its fusion) |

  Random walk read as ST's DT0064 does (the Allan deviation at 1 s equals the noise density):
  better than the 3.8 mdps/√Hz typical. The TM171 outputs bias-corrected, filtered data, hence its
  better long-term bias; the fusion re-learns the ST part's. The 1.3 °C swing is too small for
  temperature coefficients (fits: gyro Y +7.7 mdps/°C, accel X/Z +0.10 / +0.15 mg/°C); a cold start
  in the cab gives a real swing. The low-g offset is at the ±12 mg limit (|g| reads 987.5 mg):
  calibrate it.
- **Test 3, bench vibration, sander, Z up (2026-11-08): fail while shaking, both IMUs (B7).**
  The plate on the sander's body, one dial setting: the sander was failing and its speed wandered
  on its own (73–83 Hz, falling to 0.8 g rms for the last 10 s of run A), so this is one rough
  level, not a controlled one. 1.5–1.8 g rms, about 80 Hz with a harmonic near 160 Hz, 2–5 × the cab level assumed in test 3 (the tractor, below, gave 0.03 g). Nothing clipped:
  low-g 4.4 g of ±16, high-g 8.8 g of ±32, gyro 33 dps of ±500. Over the 100 s of shaking:

  | | Roll / pitch at the end of the shaking | Back at rest |
  |---|---|---|
  | SFLP (fusion on) | +6.1° / −2.9°, a steady ramp | within 0.01° in under 5 s |
  | TM171 fusion | +3.2° / −1.9°, a steady ramp | about 10 s, settled 0.08° off |
  | Gyro alone, integrated (fusion off) | about −1° / +0.4° | |
  | Low-g accel, 10 s means | +1 to +4° / −5 to +3°, \|g\| up to 1038 mg | 987 mg again |
  | High-g accel, 10 s means | within 0.35° (part of it temperature), \|g\| 1030–1034 mg | |

  The plate barely tilted (the gyro alone), so the drift is the accels: under heavy shaking the
  low-g's mean moves (vibration rectification), the SFLP follows it, and the TM171's accel does the
  same (+2° roll). The high-g's mean does not. Gyro mean shift 25 mdps at most (pass), partly the
  sander warming the die from 26.1 to 29.7 °C. The high-g sits 5.7° from the low-g at rest: it
  needs its own calibration. Still to run: the laptop cooler, levels in between on a sander that holds its speed, Y up.
- **Test 4, tractor at rest (2026-11-08): pass, both IMUs (B5, at rest).** 5 min engine off,
  5 min idle, 5 min working revs, engine off. The engine orders put a figure on each: idle 18 mg
  rms at 15, 22.5 and 45 Hz (15 Hz crank, 900 rpm); working revs 43 mg rms at 34.9, 52.2 and 204 Hz
  (34.9 Hz crank, 2090 rpm; 52.2 Hz is 1.5 × crank, a three-cylinder's firing). No clipping.

  | Shift from engine off, roll / pitch (roll sd) | Idle, 18 mg | Working revs, 43 mg |
  |---|---|---|
  | SFLP (run B) | +0.001° / +0.002° (0.006°) | −0.004° / +0.010° (0.014°) |
  | TM171 fusion (run B) | +0.007° / +0.006° (0.013°) | −0.068° / −0.068° (0.089°) |
  | TM171 fusion (run A) | +0.052° / −0.015° (0.015°) | +0.138° / −0.039° (0.043°) |
  | Low-g accel mean (run A) | +0.003° / +0.000° | −0.010° / +0.008° |

  Gyro shift under 4 mdps for both IMUs at both speeds. At this level the low-g does not rectify.
  The SFLP hardly notices the revs; the TM171's shift and spread grow with them, about 6 × the
  SFLP's at working revs. Driving runs (turns, slopes) need the UM982 roll as the reference (K7).

### 8.5 Our own fusion in the daemon (K8)

The SFLP uses the low-g accel and the gyro only ("based on the (low-g) accelerometer and gyroscope
data processing", DS15060 §2.8), so a shock that clips the low-g reaches it clipped. Read the raw
channels instead and fuse in `aow-hatd`; that can do what no external IMU can (TM171, BNO085, SFLP):

1. **Both accels.** Low-g normally; high-g for the samples where low-g is clipped; the accel weighted
   down while high-g shows a shock. The high-g is 16 × noisier (1000 against 60 µg/√Hz), but under
   heavy shaking the low-g's mean moves several degrees and the high-g's does not (§8.4, test 3):
   there the high-g, averaged over seconds and calibrated against the low-g at rest, is the better
   gravity.
2. **Adaptive accel weight.** When |a| is far from 1 g, or noisy over the last fraction of a second,
   lean on the gyro. The gyro's symmetric, fully differential architecture ("enhanced vibration
   immunity", DS15060 §1) is what makes that safe; the bench vibration runs check its bias does not
   move while shaking.
3. **GNSS aiding.** In a turn the centripetal acceleration (speed × yaw rate) tips the measured
   gravity sideways and a stand-alone IMU reads it as roll; a speed change does the same to pitch.
   The daemon has speed and heading, so it subtracts both, and re-learns the gyro bias whenever the
   vehicle is stopped.
4. **The SFLP alongside**, as a cross-check and fallback. The FIFO carries raw low-g, high-g, gyro,
   timestamps and the SFLP output in one stream; the daemon reads it in batches at about 100 Hz,
   not per sample.

Start with a complementary (Mahony-type) filter with the adaptive weighting; an EKF only if the
logs say it is needed. Order:

1. **Offline, in Python, against the logs.** The fusion-off bench runs (test 3) and the tractor
   runs (test 4) replayed through the filter and compared with the SFLP and the TM171 on the same
   run. The tractor's turns and speed changes are what item 3 is for, and need the UM982 roll (K7)
   as the truth.
2. **In `core`** (no hardware headers, unit-tested on recorded data, as the daemon rules require),
   with the SPI driver and FIFO reading in `boards/hat` behind `hal/`.

**First offline try, the sander run (2026-11-08).** `hardware/tools/imu-eval/fusion_offline.py` on
test 3's fusion-off log (476 Hz, 93 s at 1.6 g rms): Mahony-type filters on the gravity vector, the
gyro bias and the high-g's offset learnt in the still minute before, the bias re-learnt online only
while not shaking (low-g rms under 100 mg). Error against the tilt at rest before the shaking:

| Filter | Worst roll / pitch while shaking | 60 s after |
|---|---|---|
| Gyro alone | −1.0° / −0.3° | −1.5° / −1.9°, still drifting |
| Low-g, tau 1 s (as the SFLP; the SFLP itself reached +6°) | +5.0° / −10.7° | −0.06° / −0.03° |
| High-g while shaking, raw samples | −5.1° / +4.4° | −0.04° / −0.06° |
| High-g while shaking, accels low-passed 0.5 s first | −0.56° / +0.37° | −0.05° / −0.02° |
| The same, the high-g's offset corrected for temperature | −0.44° / +0.10° (−0.19° / −0.09° at the end) | −0.06° / −0.01° |

What it says for the filter in `core`:

1. **Average the accel linearly before anything nonlinear.** Normalising each sample or taking
   its cross product rectifies the shaking in the filter itself: the switch to high-g went from
   −5° to −0.56° on this alone.
2. **High-g while shaking.** Low-passing does not save the low-g (+4° / −10°): its own reading
   rectifies.
3. **Calibrate the high-g's offset against temperature.** It moved about 2 mg/°C on X and Y (0.1°
   of tilt per °C) as the sander warmed the die 3 °C; a unit can learn it over its standstills. The
   temperature row is fitted on the same run, so it is a best case.
4. **Learn the gyro bias only while quiet.** Alone, the gyro drifts 1.5–2° within a minute
   (the vibration shift and the warming).

Not yet within the 0.2° mark while shaking, at 2–5 × the vibration level of the tractor, where the
SFLP already holds 0.01°. One run, one rough level, Z up, an on/off switch at 100 mg; the plate's
tilt assumed constant (it moved under 0.06° between the still stretches).

**Smooth weighting in place of the switch (2026-11-08).** Gravity from w × low-g + (1 − w) × high-g,
both low-passed 0.5 s, w = 1 / (1 + (rms / half)²) with rms the low-g's own shaking level; the gyro
bias learnt at the rate w. Scored on the sander run and on the tractor's fusion-off run (test 4,
run A, `--shake` for idle and working revs). Worst roll / pitch:

| Filter | Sander, 1.6 g | Tractor idle, 18 mg | Tractor working revs, 43 mg |
|---|---|---|---|
| Low-g alone, tau 1 s (as the SFLP) | +5.0° / −10.7° | +0.004° / −0.025° | −0.019° / +0.051° |
| Switch at 100 mg | −0.56° / +0.37° | +0.001° / −0.021° | +0.007° / +0.037° |
| Blend, half at 50 mg | −0.55° / +0.36° | +0.005° / +0.020° | −0.036° / +0.024° |
| Blend, half at 100 mg | −0.54° / +0.35° | +0.001° / −0.021° | +0.000° / +0.037° |
| Blend at 50 mg, the pull weakened while shaking | −0.65° / +0.15° | +0.005° / +0.020° | −0.035° / +0.020° |
| Gyro alone | −1.0° / −0.3° | +0.09° / −0.30° | +0.18° / −0.55° |

The blend does as well as the switch with no step at a threshold; its half belongs at about
100 mg, since at 43 mg the low-g does not rectify and a half at 50 mg brings in 40 % of the
noisier high-g for nothing. Leaning on the gyro while shaking does not help: its bias moves under
the shaking too. What is left on the sander, about 0.4°, is mostly the high-g's offset drifting
with temperature, so that calibration is the next gain. Untested: 0.1–1 g, where the blend is
between the two accels (only the sander's last 10 s at 0.8 g). Next: a sander that holds its speed
(test 3), Y up, the cooler, and the tractor driving runs once K7 gives a roll reference.

### 8.6 If it wins

K1 stands: adopting it means a HAT revision with the part on the board over SPI0 and a reallocated
GPIO for its chip select, on a stiff part of the board away from the connectors (MEMS parts read
board stress as offset). The daemon side is a driver in `boards/hat` and a third IMU type in
`core/imu`, with roll from the daemon's own filter (§8.5, K8) or the SFLP, whichever tests 3 and 4
favour. One-time work besides: offset calibration and mount-angle zeroing.

### 8.7 Bring-up on a Pi (the fallback)

Without the HAT fitted (the HAT uses GPIO2/3 as outputs and keeps `i2c_arm` off; see K1).

1. `dtparam=i2c_arm=on` in `config.txt`, wire per §4, power up. `i2cdetect -y 1` must show `6b`
   (SJ3 at A) or `6a` (B).
2. `i2cget -y 1 0x6b 0x0f` reads WHO_AM_I: expect `0x75`.
3. SPI: `dtparam=spi=on`, wire per §4, `spidev0.0`, mode 0 or 3, ≤ 10 MHz. Read WHO_AM_I with
   address `0x8F` (read bit set) and compare.
4. Raw data: a short Python script over `smbus2`/`spidev` that enables accel and gyro at 240 Hz
   and logs CSV from the register map in DS15060 §9 (CTRL1 0x10 accel ODR, CTRL2 0x11 gyro ODR,
   CTRL3 0x12 IF_INC, data from 0x20 on; the SFLP quaternion, gravity and gyro bias are readable as
   registers on the embedded-function page, FUNC_CFG_ACCESS 0x01). The Linux IIO driver
   `st_lsm6dsx` does not know this part yet; ST's platform-independent C driver (`lsm6dsk320x-pid`
   on GitHub, or the sister part's `lsm6dsv320x-pid` until it exists) is the starting point.

## 9. Open items and checks

IDs: **K#** = decision or open item, **B#** = bench check. Status: open, decided, pass, fail.

| ID | Item | Status |
|---|---|---|
| **K1** | **Where the IMU goes if adopted.** All 28 HAT GPIOs are allocated and I²C1 (GPIO2/3) carries `MOT_DIR` / `WDT_EN`. Adopting this IMU on the HAT means an SPI0 chip select (none spare) or a GPIO reallocation for I²C, i.e. a HAT revision. Decide after the vibration result, not before. | open |
| **K2** | **Part availability.** JLCPCB stocks the LSM6DSK320XTR (C53025794, 81 pcs on 2026-10-06, Extended), so JLC can assemble U1. ST's own adapter STEVAL-MKI257A is announced for Nov 2026; not needed. | **closed 2026-10-06** |
| **K3** | **Register map and WHO_AM_I.** Initially designed from the data brief and the sister part's datasheet. DS15060 Rev 1 (July 2026) confirms the pin table, VDD/VDDIO ranges, I²C address, SPI timing and gives WHO_AM_I = 0x75; §2 now cites it. | **closed 2026-10-06** |
| **K4** | **Land pattern.** The 1 : 1 pattern in §5 is the usual ST LGA practice; confirm against TN0018 (st.com, "Surface mounting guidelines for MEMS sensors in an LGA package") and the stencil guidance there (90–150 µm stainless, apertures 70–90 % of the pad) before ordering. | open |
| **K5** | Whether to add a Qwiic/STEMMA QT connector (JST SH 4-pin). Left off: the test rig is a Pi header, and a second connector adds nothing to the measurement. | decided: no |
| **K6** | Header arrangement: ~~one 1 × 10 row~~ → **two 1 × 5 rows on opposite edges, 0.6" apart.** A single row leaves the board hanging off one edge; two rows make it a DIP-style module that sits flat on a breadboard or in two female headers. The mode 2/3 pins moved to inboard pads (J3). | **decided 2026-10-06** |
| **B1** | EasyEDA footprint pin numbering matches §2 (pin 1 top-right in the **bottom** view, i.e. top-left seen from above). | open |
| **B2** | `i2cdetect` shows 0x6B at SJ3 = A and 0x6A at B. | open |
| **B3** | SPI WHO_AM_I at 10 MHz, modes 0 and 3. | open |
| **B4** | SJ1 open, VDDIO at 1.8 V: I²C and SPI still work. Optional. | open |
| **K7** | **The UM982's dual-antenna roll in `/api/status`**, as the vibration-free reference for the tractor runs (§8.3 test 4). A small daemon change. | open |
| **B5** | Vibration comparison on the tractor against the TM171 or BNO085 (§8.3 test 4). | **at rest pass 2026-11-08:** at working revs SFLP within 0.010°, sd 0.014° (TM171 0.07–0.14°, 0.089°); driving runs open (K7) |
| **B6** | Still baseline: two hours at rest, Allan deviation, random walk near 0.23 °/√h (§8.3 test 2). | **pass 2026-11-06:** 0.14 °/√h, tilt within 0.011° over 2 h (§8.4) |
| **B7** | Bench vibration: still-to-shaking shifts within the §8.3 table, two orientations (§8.3 test 3). | open: sander Z up 2026-11-08 fails while shaking for both IMUs, SFLP +6°, TM171 +3°, the low-g rectifying (§8.4); cooler and Y up to run |
| **K8** | **The daemon's own fusion** from the raw low-g, high-g and gyro, GNSS-aided (§8.5). Offline in Python against the test 3 and 4 logs first, then in `core`. | open: offline 2026-11-08, sander run held to 0.4–0.6° against the SFLP's 6°, tractor within 0.04°; low-g/high-g blend, half at 100 mg (§8.5) |
