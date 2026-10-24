# Plan: virtual terminal

> The implement's screens in AgOpenWeb. Rewritten 2026-10-24: the terminal runs **inside
> AgOpenWeb**, on AgIsoStack++ through a .NET wrapper, and this repo provides the **CAN gateway**
> that carries an implement bus to it. The first plan (2026-10-13: an `aow-vtd` service here,
> drawing in a browser over a WebSocket) is dropped; it is in git history before this date.
> The daemon plan is `docs/PLAN.md`; its section 8 covers the ISOBUS work that is built.

## 1. The shape

```
implement ── CAN ── gateway ── TCP (Wi-Fi or loopback) ── AgOpenWeb ── terminal (AgIsoStack++ VT server)
                    │                                                  via AgIsoStack-dotnet
                    ├─ HAT: hatcan2, a service on the CM4 (this repo)
                    ├─ any Linux box with a CAN interface (the same service)
                    └─ an ESP32 DIY gateway, straight to Wi-Fi (not this repo)
```

The terminal is a real ISOBUS terminal in the app, so it is the same on every platform AgOpenWeb
runs on: the headless instance on the CM4 (gateway on loopback), and the tablet apps (gateway
over Wi-Fi). Every CAN frame crosses the link with the bus's timing. SolSteer View-T ships that
way (section 7), and our bench (section 4) shows it holds inside the ISOBUS timers.

| Decision | Choice | Why |
|---|---|---|
| Where the terminal runs | In AgOpenWeb (C#), not a service here | One terminal on every platform; the screens are drawn by the app that shows them |
| Stack | AgIsoStack++'s `VirtualTerminalServer`, through **AgIsoStack-dotnet** (below) | Upstream's server, kept at upstream HEAD |
| Wrapper | A C interface P/Invoked from C#, in its own MIT repo: `/Users/chris/Code/AgIsoStack-dotnet`, <https://github.com/chriskinal/AgIsoStack-dotnet> (private for now; to be offered to Open-Agriculture) | C++/CLI is Windows only. MIT keeps it free of this repo's GPL |
| Link | TCP, Nagle off; frames batched per read | Ordered and repaired by the link; loss up to 5 % measured harmless (section 4) |
| This repo's part | The gateway on the HAT and on any Linux CAN box | The terminal needs a bus; the HAT has one wired |

## 2. Who does what

| Repo | What |
|---|---|
| AgIsoStack-dotnet | The wrapper: one-thread C interface over the stack, its tests, the Wi-Fi bench (relay, netem). Its `docs/PLAN.md` (the real API next) and `docs/RESULTS.md` |
| AgOpenWeb | The terminal in the app: hosting the server, drawing the object tree, operator input, stored pools, the card. App code: its plan belongs in that repo and is not written yet |
| AgOpenWeb-HAT (here) | The gateway service (section 3), its settings and status, packaging |
| isobus-implement-sim | Implements to test against (step −1, below) |

**Step −1: implement simulators (2026-10-22).** We own no ISOBUS implements. A separate MIT
repository, `isobus-implement-sim` (<https://github.com/chriskinal/isobus-implement-sim>, private
for now), builds them from profile files, with faults on demand. Testing against a real
implement still comes before a release: a simulator only proves we match our own reading of the
standard.

## 3. The gateway (this repo; not built)

The bench's `bench/can_tcp_relay.py` (in AgIsoStack-dotnet) is the starting point: one CAN
interface to one TCP client, a fixed 18-byte record per frame with a microsecond timestamp, and a
ping record the client times the link with.

- **Process.** Its own small service beside `aow-hatd`, like `aow-tcd` (`docs/PLAN.md` section 8):
  it parses nothing, but restarts independently and runs on a Linux box without the HAT. SocketCAN
  lets it share `hatcan2` with `aow-hatd`'s own ISOBUS work. Normal priority (the control loop
  measured unaffected by a saturated CAN2, `docs/PLAN.md` section 4).
- **Drop stale frames.** After a link cut TCP delivers every frame queued during it, so a soft
  key pressed during a 10 s cut reaches the bus 10 s late. The gateway drops frames older than a
  limit in both directions (the records carry timestamps); the limit is an open item.
- **Off by default.** A tractor with its own terminal would otherwise see two.
- **Status** to `aow-hatd` on `127.0.0.1:8891` like `aow-tcd`: running, client connected, frames.

## 4. What the bench has shown (2026-10-24)

Details in AgIsoStack-dotnet's `docs/RESULTS.md`. The wrapper's terminal on a Mac, a Galaxy Tab
S7 FE and an iPad Pro; the relay on the dev CM4's CAN2, upstream's VT example implement on CAN3
(wired to CAN2) uploading upstream's 150 kB example pool:

- **Speed is set by the implement's packets per DPO.** At 255: 14.5-14.7 s on home Wi-Fi, 13.8 s
  on the CM4's own access point (the bus alone needs about 12 s). At 16: 34-55 s, a round trip
  per 16 frames.
- **Delay and loss** (netem, both ways): 200 ms round trip adds about 21 s to that pool (one round
  trip per batch); 5 % loss adds about 2 s; 50 ms ±25 with 3 % loss adds about 17 s. All inside
  the timers.
- **Link cuts:** 0.5 s is ridden through. 1 s or more mid-transfer aborts it (ETP T2 1.25 s) and
  the implement starts the whole pool again; 3 s or more also resets VT connections. 10 s
  recovers on its own: TCP holds the connection.
- **Two bugs found by capture, fixed here:** the stack's transmit queue lost frames with two
  producers (upstream #656, PR #793, carried as a patch); `aow-hatd`'s VT client dropped a
  terminal whose upload ran past 10 s (0.2.23).
- **A shutdown crash in the stack's VT server**, reported upstream as #802 (2026-10-24); the
  wrapper works around it.

## 5. Rules for the terminal, wherever it runs

- **A press is always followed by a release.** If the link drops while a button is held, the
  terminal side sends the release once the link is back; the gateway's stale-frame limit keeps a
  late press off the bus.
- **One operator.** Any number of screens may watch; one at a time may press.
- **An unsupported object is drawn as a labelled box**, never dropped silently and never a
  reason to refuse the pool.
- **A bad pool is refused, not survived.** The stack reports the faulting object.
- **Alarms reach the operator** with the terminal's card closed.
- **Stored pools** by the implement's full 64-bit NAME and version label.
- **Screen size** 480 x 480 to start, scaled for display (espVT's finding agrees: an implement
  fills more only if it scales its own pool).

## 6. Open items

1. **Identity on the bus.** A terminal claims an address with a manufacturer code; the wrapper
   uses 1407, as the stack's examples do. The same question is open for the task controller.
2. **Two terminals on one bus.** Allowed, but which one an implement picks varies by implement.
3. **The gateway protocol.** The bench's record format, or GVRET: what ESP32RET-style DIY Wi-Fi
   gateways and SavvyCAN already speak. `aow-hatd`'s GVRET on TCP 23 is listen-only and streams
   the brand CAN through the control loop's ring, so it is not the gateway as it stands. Decide
   with the ESP32 gateway in view; the stale-frame limit and a ping need a place either way.
4. **The stale-frame limit**: shorter than any held-button risk, longer than a Wi-Fi retry burst.
   1 s to start.
5. **Fonts.** The standard's fonts are fixed-size bitmap cells; espVT draws every size with one
   scaled monospace font. Decide in the app.
6. **AUX-N** (joystick assignment) is a headline feature of both products in section 7; the stack
   has it since #655.

## 7. Prior art

**SolSteer View-T** (<https://www.solsteer.com/view-t>, read 2026-10-13, not tested by us). A
commercial tablet terminal: an iOS or Android app with a licence key, and a "CAN wireless module"
on the in-cab ISOBUS socket that is a Wi-Fi access point. The terminal runs in the app and every
CAN frame crosses one Wi-Fi hop, the shape this plan now takes. It also assigns AUX-N functions
and records CAN traces.

**espVT** (<https://github.com/MatNav1/espVT>, read 2026-10-22 at af8de3b, not tested by us). A
terminal server on an ESP32-S3 with a 7 inch touch LCD, MIT, experimental: draws the implement's
screens itself (about 15 object types in about 3,000 lines), AUX-N Type 2, alarm sounds, stored
pools in flash, GNSS with NTRIP. Its copy of AgIsoStack++ (509a920) is changed in place in 14
files. Gaps it found that were still open upstream on 2026-10-19, and where they stand for us:

| # | Gap | espVT's change | For us |
|---|---|---|---|
| 1 | After the terminal restarts, an implement carries on with maintenance messages without the init bit, and the server never takes it on | Accept it; load its stored pool if there is one version | open (stored pools) |
| 2 | Status messages start at once after a restart, so an implement never sees the terminal go away | No status for the first 5 s | open |
| 3 | Changing the active mask does not run the pool's On Show and On Hide macros | Run them, then On Change Active Mask | open |
| 4 | The transmit queue's `push` is safe for one producer only | A mutex on `push` | upstream PR #793; the wrapper has one thread and does not use that queue |
| 5 | `get_object_by_id` uses `operator[]`, so a lookup of a missing id adds an empty entry | `find()` | open |
| 6 | The working-set list is read and changed from two threads with no lock | A recursive mutex | the wrapper runs the server on one thread |
| 7 | Control Audio Signal and Set Audio Volume are acknowledged and dropped | Two virtual functions | open (alarms) |
| 8 | Every Change Numeric Value repaints, changed or not (upstream #796) | Repaint only on a change | open |
| 9 | A long ETP stream during an upload is drained in one pass, starving other work | At most 32 per pass | not seen on the bench |

Each open gap wants its own upstream pull request with a test.
