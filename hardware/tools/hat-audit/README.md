<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The AgOpenWeb-HAT contributors -->

# HAT audit scripts

Python scripts that read the EasyEDA Standard JSON exports of the AOW HAT and check them.
They import each other, so keep them in one folder. The plotting scripts need `matplotlib`.

Inputs come from environment variables (defaults point at `~/Downloads`):

| Variable | Meaning |
|---|---|
| `HAT_SCH` | schematic export (`SCH_AOW-HAT_*.json`) |
| `HAT_PCB` | PCB export (`PCB_PCB_AOW-HAT_*.json`) |
| `HAT_OUT` | output PNG for `png.py` |

| Script | What it does |
|---|---|
| `sch.py` | parses the schematic; lists pins with no net or more than one |
| `doccheck.py` | schematic export against `HARDWARE_HAT_NETLIST.md` |
| `pcb.py` | parses the PCB; compares pad nets with the schematic |
| `conn.py` | routing completeness: nets still split into islands |
| `rats.py` | ratsnest length, crossing count, nets crossing the J1 row |
| `plan.py` | moves a block of parts; checks overlaps, standoff keep-outs, board edge |
| `opt.py` | searches free space for the best position of each block |
| `final.py` | the 2026-09-26 move plan, scored |
| `png.py` | draws the move plan |
| `hatcheck.py` | EasyEDA `.net` export against `HARDWARE_HAT_NETLIST.md` |
| `place.py` | placement check: each passive against its owner, keep-outs |

```bash
python3 -m venv venv && ./venv/bin/pip install matplotlib
HAT_SCH=~/Downloads/SCH_x.json HAT_PCB=~/Downloads/PCB_x.json ./venv/bin/python conn.py
```

The move plan is checked for overlaps, keep-outs and straight-line net length only. It is not routed.
