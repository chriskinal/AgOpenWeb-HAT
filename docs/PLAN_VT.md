# Plan: virtual terminal

> The implement's screens in AgOpenWeb. Since 2026-10-24 the terminal runs **inside AgOpenWeb**,
> on AgIsoStack++ through the **AgIsoStack-dotnet** wrapper, and is fed every CAN frame over TCP
> by a gateway. This repo's part is that **gateway on the HAT**: `aow-linkd`, built 2026-10-25.
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

## 2. The gateway service, `aow-linkd` (built 2026-10-25, 0.2.24)

Implements the AOW link protocol, version 1 (AgIsoStack-dotnet's `docs/TRANSPORT.md` section 5:
our own protocol, not GVRET; hello, heartbeat, bus status, the 1 s silence rule).

- **The protocol** is `core/src/aow_link.cpp` (`aow/aow_link.h`): transport-free like `gvret`,
  bytes and frames in, bytes out, the time handed in. One control client, up to four monitors,
  the bus drained when a control client connects, frames to the bus dropped after 100 ms, STATUS
  every second and on a change. Unit tests: `tests/test_aow_link.cpp`.
- **The program** is `daemon/aow_linkd.cpp`: one thread, one poll loop over the listener, the
  CAN sockets (SocketCAN, kernel timestamps, the receive-overflow count) and the clients. Bus
  state, error counters and bitrate from rtnetlink once a second; an interface missing at start
  is looked for every second. `--can IF[,IF...]` (default `hatcan2`), `--listen` (default
  127.0.0.1), `--port` (2323), `--name`.
- **Its own service beside `aow-hatd`**, as planned: `deploy/aow-linkd.service`, `Type=notify`
  with `WatchdogSec=5` (pinged from the loop), `Restart=always`, `DynamicUser` (SocketCAN and
  rtnetlink need no privileges), normal priority, no dependency on `aow-hatd` either way.
  SocketCAN shares `hatcan2` with `aow-hatd`'s ISOBUS work: on the bench the task controller's
  own status screen (0xF7) reached the terminal through it.
- **Off by default.** The ISOBUS page's switch (below), or `install.sh --hat --gateway` /
  `--no-gateway`; neither flag leaves it as it is (so an update keeps the choice). Its name and
  where it listens are set on the ISOBUS page (below, 0.2.28). Per-unit options in
  `/etc/aow-hat/linkd.env` (`AOW_LINKD_OPTS`), kept across updates, still win over the page.
- **Packaged** in the `.aowpkg`; the updater refuses a package without it.
- **Tested:** `tests/link_probe.py build/aow-linkd [aowlink_check.py]` on vcan (frames both ways,
  the drain, STATUS following the interface down and up, a late interface); AgIsoStack-dotnet's
  `bench/aowlink_check.py` (all checks), its `bench/local.sh` with `GATEWAY=` (the bench and a
  1.5 s cut, in a container), and its Wi-Fi bench on the dev CM4 with the service (both example
  implements, Mac terminal: AgIsoStack-dotnet `docs/RESULTS.md`).
- **Its status in `aow-hatd`** (0.2.25): PGN 153 from source 0x81 to `127.0.0.1:8891`
  (`aow/helper_status.h`; 150 to 153 are free in AgOpenGPS's PGN list and unused by AgOpenWeb),
  once a second and at once when a terminal connects or goes: listening where and on which
  port, the terminal's name and address, frames both ways and lost since it connected, monitors,
  each channel's interface, bitrate, state and error counters. `aow-hatd` listens in its web
  thread, not the control thread, and only shows it: `linkGateway` in `/api/status` and
  `/api/isobus`, a Virtual Terminal Gateway card on the ISOBUS page, a log line on each change.
  3 s without a report: not running. On the CM4: off, idle, a terminal mid-upload, stopped.
- **Its switch on the ISOBUS page** (0.2.26): a toggle on the gateway card, acting at once
  (`POST /api/linkgateway {"enabled": bool}`: `aow-hatd`, root, runs `systemctl --no-block
  enable --now` or `disable --now`), apart from Apply Changes: no settings saved, nothing else
  restarted. On or off is systemd's enablement, read from its link (no `systemctl` for the
  page's once-a-second poll), so the page, `install.sh --gateway` / `--no-gateway` and `systemctl`
  all agree, and an update keeps it. Hidden where the gateway is not installed; switching off
  with a terminal connected asks first. On the CM4: on, reporting within 2 s; off, stopped and
  disabled within about 4 s.
- **Its name and the network, and finding it** (0.2.28; AgOpenWeb `Plans/ISOBUS_VT_TRANSPORTS.md`
  step 2): on the gateway card, a name ("Gateway Name", default "AOW HAT" and the host name) and
  "Tablets on the Network" (off: loopback only; on: every network, `0.0.0.0`), saved with their
  own button (`POST /api/linkgateway {"name", "network"}`): `aow-hatd` writes
  `/etc/aow-hat/linkd.conf` (`name=`, `network=`; `aow-linkd --config` reads it before its other
  options) and restarts the gateway if it runs. While the gateway is on and listens on the network,
  `aow-hatd` keeps an avahi service file, `/etc/avahi/services/aow-linkd.service`, announcing it
  by DNS-SD as `_aowlink._tcp` with that name (TXT `v=1`): AgOpenWeb's VT card lists it by name
  (AgIsoStack-dotnet `AowLinkBrowser`). The file follows the switch and the settings, and is put
  right when `aow-hatd` starts. The name also goes in the gateway's HELLO. No pairing code: the
  machine network is trusted, as for the steer PGNs (AgIsoStack-dotnet `docs/TRANSPORT.md`,
  Security); the help text says to use it on the tractor's own network. A second terminal is
  refused with CLOSE busy "in use by" the first one's name, for the card to show.
- **The tablets through the service** (2026-10-25, 0.2.26, switched on from the page): the iPad
  and the Galaxy Tab pass with both example implements, link up throughout (AgIsoStack-dotnet
  `docs/RESULTS.md`). One run showed `aow-hatd`'s status-screen RTS reaching the terminal twice,
  as once before with the Python relay: its open observations say how to catch it.

## 3. Found on the bench, fixed here

The Wi-Fi bench (2026-10-24, AgIsoStack-dotnet `docs/RESULTS.md`) found two `aow-hatd` bugs,
fixed in 0.2.23 (8bb10f1): the stack's transmit queue lost frames with two producers (upstream
#656, PR #793 carried as `third_party/agisostack-patches/pr793-transmit-queue.patch`), and the
VT client dropped a terminal whose upload ran past 10 s (`tests/isobus_vt_probe.py` step 7).

A third, fixed in 0.2.27 (2026-10-25): now and then `aow-hatd` sent a transfer's request-to-send
twice, back to back, and the terminal's answer to both made it abort; its status screen was
uploaded again. Caught with a capture of the bus, and reproduced on vcan: a race in the stack
between our aow-isobus thread starting a transfer and the hardware thread updating it (upstream
issue #807, our PR #808, carried as `third_party/agisostack-patches/pr808-transport-first-frame.patch`
with the pin moved to e932041). AgIsoStack-dotnet `docs/PLAN.md`, upstream gaps, has the detail.
