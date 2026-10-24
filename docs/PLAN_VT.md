# Plan: virtual terminal

> The implement's screens in AgOpenWeb. Since 2026-10-24 the terminal runs **inside AgOpenWeb**,
> on AgIsoStack++ through the **AgIsoStack-dotnet** wrapper, and is fed every CAN frame over TCP
> by a gateway. This repo's part is that **gateway on the HAT**. Nothing here is built.
> The first plan (2026-10-13: an `aow-vtd` service here, drawing in a browser over a WebSocket)
> is dropped; it is in git history before 17ad56e.

## 1. Where things are

| Repo | What |
|---|---|
| AgIsoStack-dotnet (`/Users/chris/Code/AgIsoStack-dotnet`, <https://github.com/chriskinal/AgIsoStack-dotnet>, private, MIT) | The wrapper, its tests, the Wi-Fi bench, and the **transport**: the gateway protocol (`docs/TRANSPORT.md`), the bench setup on this repo's dev CM4 (`docs/BENCH.md`), every measurement (`docs/RESULTS.md`), the terminal's rules, the drawing design and the upstream VT server gaps (`docs/PLAN.md`) |
| AgOpenWeb | The terminal in the app: hosting the server, drawing, input, stored pools. Its plan is not written yet |
| AgOpenWeb-HAT (here) | The gateway service on the CM4 (section 2) |
| isobus-implement-sim | Implements to test against (step −1) |

```
implement ── CAN2 (hatcan2) ── gateway service ── TCP (Wi-Fi, or loopback for AgOpenWeb on the CM4) ── AgOpenWeb terminal
```

**Step −1: implement simulators (2026-10-22).** We own no ISOBUS implements; `isobus-implement-sim`
(<https://github.com/chriskinal/isobus-implement-sim>, private) builds them. A real implement
still comes before a release.

## 2. The gateway service (not built)

Implements the protocol in AgIsoStack-dotnet's `docs/TRANSPORT.md` (still to be settled there:
our own or GVRET with additions; hello, heartbeat, bus status, a stale-frame limit). The bench's
Python relay in that repo is the working prototype.

- **Its own service beside `aow-hatd`**, like `aow-tcd` (`docs/PLAN.md` section 8): it parses
  nothing, but restarts on its own and runs on a Linux box without the HAT too. SocketCAN lets it
  share `hatcan2` with `aow-hatd`'s ISOBUS work. Normal priority: a saturated CAN2 left the
  control loop's timing unchanged (`docs/PLAN.md` section 4).
- **Not GVRET on TCP 23 as it stands:** `aow-hatd`'s GVRET is listen-only and carries the brand
  CAN through the control loop's ring.
- **Off by default.** A tractor with its own terminal would otherwise see two.
- **Status** to `aow-hatd` on `127.0.0.1:8891` like `aow-tcd`: running, client connected, frames,
  bus state; shown on the ISOBUS page and in `/api/status`.
- **Packaging** in the `.aowpkg`, with an installer option and its settings.

## 3. Found on the bench, fixed here

The Wi-Fi bench (2026-10-24, AgIsoStack-dotnet `docs/RESULTS.md`) found two `aow-hatd` bugs,
fixed in 0.2.23 (8bb10f1): the stack's transmit queue lost frames with two producers (upstream
#656, PR #793 carried as `third_party/agisostack-patches/pr793-transmit-queue.patch`), and the
VT client dropped a terminal whose upload ran past 10 s (`tests/isobus_vt_probe.py` step 7).
