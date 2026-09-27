# AgOpenWeb HAT — Issues and Checks

> Open decisions, checks still to do, and results so far.
>
> **Part of the HAT document set:**
> - `HARDWARE_HAT_DESIGN.md`: why
> - `HARDWARE_HAT_NETLIST.md`: wiring
> - `HARDWARE_HAT_BOM.md`: parts
> - this file: open items
>
> Ref designators match across all four.
>
> IDs: **H#** = a design decision or open item; **B#** = a bench check.
> Status: **open**, **decided**, **pass**, **fail**.

## 1. Open decisions and items

| ID | Item | Proposal | Status |
|---|---|---|---|
| **H1** | Vehicle power path | ~~J4 on the HAT, switched through Q2 to the IO board~~ → **12 V straight to the IO board's J20**. The HAT switches only its own 5 V (Q2) and holds the CM4 off via `GLOBAL_EN` (Q1). J4, J5 and the reverse-polarity stage removed; J3.1 becomes a 12 V sense wire. | **decided 2026-09-25:** direct power (design §4) |
| **H2** | CAN transceiver current | The 5 V budget assumes ~70 mA per MCP251863 transceiver when dominant (design §4.7). Check the datasheet figure. | open |
| **H3** | GNSS board dimensions | Measure on real boards: the EMAX's square hole pattern (36.0 × 36.0 on H5/H9/H10; its odd fourth hole is unused) and its left-edge connector; the UM982EB's antenna-end hole spacing (34.3 mm in the part, matching v5.0; another footprint says 34.0) and the distance from those holes to its header (64.6 mm). With Ø 3.3 holes and M3 screws there's only ~±0.15 mm of play. | open |
| **H4** | GNSS UART logic level | Confirm both boards' TX1/RX1 are 3.3 V, not shifted to 5 V. | open |
| **H6** | NVMe standby draw | The 150 µA IO-board figure (B13) — check whether an NVMe in the PCIe slot adds to it; the slot's 3.3 V converter runs from 12 V. | open |
| **H5** | LCSC stock and fees | Re-check stock at order time (the 2026-09-23 figures are a snapshot), especially J7 (C22436146, ~1.1k). Confirm whether "Pref-Ext" parts carry the extended fee. | open |
| **H7** | `SW_REMOTE` on GPIO2 can't go low | GPIO2 (J1.3) has a 1.8 kΩ pull-up on the CM4. With R30 at 1 kΩ, a closed switch gives ~1.3 V, not a valid low. Move `SW_REMOTE` to another GPIO and put a driven signal on GPIO2 (a CAN interrupt or `MOT_DIR`). | open (audit 2026-09-26) |
| **H8** | GPIO swap for routing | Switches on pins 11/13/15, `CAN2_INT`/`CAN3_INT` on pins 35/38, `MOT_DIR` on pin 3, `PIEZO_PWM` on pin 16, `WDT_WDI` on pin 31. Cuts ratsnest crossings from 446 to 350 (with H11) and fixes H7. Needs schematic, software pin map and doc changes. | open |
| **H9** | D2 / D5 leakage | The B5819W Schottky clamps leak when hot and can lift `KEY_SENSE` toward the 0.61 V key-on threshold. The dividers never exceed ~3 V, so leave D2/D5 unfitted or fit 1N4148W (C81598). | open |
| **H10** | J1.17 on `CM4_3V3` | J1.1 and J1.17 are the same rail on the IO board. Drop J1.17 from the net to save a route. | open |
| **H11** | Placement moves for routing | U7 block to X 38–50, Y −28…−38; X1 block into U7's old spot; U4 block to X 43–48, Y −55…−65; U11 below J1 beside R39; SW1 beside U10; R27 beside C33. Keep X 33–38 below J1 clear. Checked for overlaps and net length only, not routed. Scripts: `tools/hat-audit/`. | open |
| **H12** | Routing rules | The 2026-09-26 PCB has no GND pour and uses 0.125 mm tracks / 0.10 mm clearance everywhere, power included; 23 of 98 nets are incomplete. Pour GND on the bottom layer, use 0.2 / 0.2 mm with 0.5 mm power, and hand-route power, SPI, CAN pairs and clock first. Consider 4 layers. | open |
| **H13** | J3 and H10 positions | On the PCB J3 pin 1 is at X 3.93 (design §2: 4.63) and H10 is at Y −40.0 (design §2: −39.5). Confirm which is intended. | open |
| **H14** | WAS / current-sense input protection | D13/D14 (ESD9B5V) sit before R26/R27, so a 12 V short on J3.4 or J3.6 would destroy them. Accept, or note it in the harness doc. | open |

## 2. Bench checks

### 2.1 Done: CM4 IO Board, Raspberry Pi OS, bootloader 2023-01-11 (`8ba17717`), 2026-09-23

| ID | Check | Result |
|---|---|---|
| **B1** | `CM4_3V3` (40-pin header pin 1) falls at halt with `POWER_OFF_ON_HALT=1`, `WAKE_ON_GPIO=0` | **pass.** Drops to 0 V. With the stock `POWER_OFF_ON_HALT=0` it stays up, so the EEPROM setting is required. The IO board schematic also shows `RUN_PG` at 0 V while halted. |
| **B2** | `CM4_3V3` doesn't dip across `sudo reboot` or a `RUN_PG` reset | **pass.** No dip on reboot, or on a ~0.2 s tap of the IO board's J1 pad 3 (`RUN_PG`) to pad 2, which simulates U10's ~210 ms pulse. The 1.2–2.6 s hold is pure margin. |
| **B13** | IO board current at 12 V with the CM4 halted (`POWER_OFF_ON_HALT=1`) | **pass (2026-09-25): ~150 µA**, which made direct power viable (H1). NVMe fitted? See H6. |
| **B3** | A `GLOBAL_EN` pulse restarts a halted CM4 with 5 V still on | **pass.** A short tap and a multi-second hold (it boots on release) both work. Done through the IO board's J2 pins 13–14, its own wake button, which pulls `GLOBAL_EN` to `RUN_PG` (0 V while halted). |

Setup note: writing the EEPROM from Linux on a CM4 needs a `[cm4]` block in `config.txt`, and on
this bench that lost the saved Wi-Fi profile. Use `rpiboot` instead (design §7).

### 2.2 To do before trusting it in a tractor

| ID | Check | Why | Status |
|---|---|---|---|
| **B4** | **Crank dip on the IO board:** run the IO board + HAT from a bench supply stepped down to 8, 7, 6 V for 1–5 s | the IO board is **rated down to 7.5 V**; a hard crank may reset it | open |
| **B5** | **Meter the IO board's J20** before building the J5 lead: pin 1 should read +12 V (barrel jack powered) | J20's pin order is the reverse of floppy-cable colours | open |
| **B6** | **Latch off-state:** key off, CM4 halted → `5V_MAIN` at 0 V and `GLOBAL_EN` held low (the CM4 stays off) | | open, needs the built HAT |
| **B7** | **Standby draw** of the whole system with the key off, compared with the ≈ 0.31 mA estimate (design §4.5) | | open, needs the built HAT |
| **B14** | **Battery connect with the key off:** the CM4 must not boot (Q1 holds `GLOBAL_EN` low from power-up). If it does start briefly, it should shut itself down | power-up race between the IO board's 5 V and Q1 | open, needs the built HAT |
| **B8** | **Low-voltage cut-off:** trips at 11.5 V ± 0.2 V, only 6–13 s after key-off | | open, needs the built HAT |
| **B9** | **Restart:** key off → on during shutdown, and a UI shutdown with the key on, both restart the CM4 | block E | open, needs the built HAT |
| **B10** | **Crank test on a machine** with the keyed feed on IGN/RUN: no dropouts, no false trips | | open |
| **B11** | **Shutdown time** on the production image (key-off → halt) | the key monitor and the UI should know it | open |
| **B12** | **5 V current** on the built system (the CM4 busy, CAN traffic, GNSS running) against the ≈ 1.6 A typical / 2.77 A worst estimate | | open |
| **B15** | **Back-powering at halt:** with the HAT fitted and the key off, `CM4_3V3` falls to 0 V after halt and the latch releases | U9 and the CAN chips still drive high into the unpowered CM4 pins, which may hold `CM4_3V3` and `HOLD_G` up | open, needs the built HAT |
