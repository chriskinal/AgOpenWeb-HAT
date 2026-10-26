<!-- SPDX-License-Identifier: CERN-OHL-P-2.0 -->
<!-- Copyright (c) 2026 The AgOpenWeb-HAT contributors -->

# IMU Breakout — ST LSM6DSK320X

> A small breakout board for evaluating the **ST LSM6DSK320X** 6-axis IMU (gyro + low-g and high-g
> accelerometers, "enhanced vibration immunity") as a candidate roll/heading sensor for AgOpenWeb.
> It mounts rigidly with two M3 screws and plugs into a Raspberry Pi 40-pin header (CM4 IO Board or
> any Pi) over I²C or SPI at 3.3 V.
>
> **Status (2026-10-07): parked.** Designed on paper, not drawn. Two assembled boards from JLC come
> to about $125; ST's own DIL24 adapter for this part, **STEVAL-MKI257A** (announced for Nov 2026),
> is the usual $15–30 and carries the same circuit (sensor, 100 nF, 10 µF), so the plan is to wait
> for it. Kept as the reference if the adapter slips or a bolt-down board is needed for the
> vibration test (the DIL24 module has no mounting holes and no pull-ups: a Pi's I²C1 has its own).
> This file is the complete document set for the board: design, netlist, BOM and open items.
> Designed for **EasyEDA Standard**, 2 layers.
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

## 8. Bring-up on the CM4 IO Board

Without the HAT fitted (the HAT uses GPIO2/3 as outputs and keeps `i2c_arm` off; see K1).

1. `dtparam=i2c_arm=on` in `config.txt`, wire per §4, power up. `i2cdetect -y 1` must show `6b`
   (SJ3 at A) or `6a` (B).
2. `i2cget -y 1 0x6b 0x0f` reads WHO_AM_I: expect `0x75`.
3. SPI: `dtparam=spi=on`, wire per §4, `spidev0.0`, mode 0 or 3, ≤ 10 MHz. Read WHO_AM_I with
   address `0x8F` (read bit set) and compare.
4. Raw data: a short Python script over `smbus2`/`spidev` that enables accel and gyro at 240 Hz
   and logs CSV from the register map in DS15060 §9 (CTRL1 0x10 accel ODR, CTRL2 0x11 gyro ODR,
   CTRL3 0x12 IF_INC, data from 0x20 on). The Linux IIO driver `st_lsm6dsx` does not know this part
   yet; ST's platform-independent C driver (`lsm6dsk320x-pid` on GitHub, or the sister part's
   `lsm6dsv320x-pid` until it exists) is the starting point.
5. Vibration test: same bracket, same log, engine off / idle / PTO speed, with the BNO085 next to
   it. Compare gyro bias drift and roll noise. This is the result the board exists for.

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
| **B5** | Vibration comparison against the BNO085 (§8 step 5). | open |
