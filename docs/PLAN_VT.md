# Plan: virtual terminal service

> A terminal on the ISOBUS that has no screen of its own: it shows the implement's screens in
> AgOpenWeb's web client. Written 2026-10-13. **Nothing here is built.**
> The daemon plan is `docs/PLAN.md`; its section 8 covers the ISOBUS work that is built.

## 1. What it is

An ISOBUS implement (sprayer, seeder, baler) carries its own operating screens and uploads them
to whatever terminal it finds on the bus. Today that is the tractor's cab terminal. This service
is a terminal on the bus that draws nothing itself. It keeps the implement's screens and current
values, and a browser draws them in a card in AgOpenWeb and sends touches back.

It is the opposite of what `daemon/isobus_vt.cpp` does. That file is a terminal *client*: it puts
the daemon's status screen on the tractor's terminal. This service is the terminal *server*.

The terminal lives on the bus whether or not a browser is looking. The service answers the
implement on time; a slow or dropped Wi-Fi link only delays the picture.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Where it runs | Its own executable, `aow-vtd`, in this repo. Not inside `aow-hatd` | A terminal parses whatever pool any implement sends. A crash in that must not stop steering. SocketCAN lets both processes use one interface |
| Stack | AgIsoStack++'s `VirtualTerminalServer`, at the commit `CMakeLists.txt` pins for `aow-hatd` too, kept close to upstream | It claims the address, takes and parses the pool, answers the implement's commands. The server class is changing fast: about 20 upstream commits between 2026-09-19 (c0e1ebb) and 2026-10-19 (42ea80e) touch it (section 10). The pin moved to 42ea80e on 2026-10-22: AOG-TaskController's file built unchanged and the three ISOBUS probes passed, so one pin serves both executables |
| CAN side | A SocketCAN interface name on the command line | `hatcan2` on the HAT, any adapter on another Linux box, or a `vcan` fed by a CAN-over-IP bridge |
| HAT needed | No | It needs Linux and a CAN interface. It never assumes `aow-hatd` is running |
| Link to `aow-hatd` and restarts | As `aow-tcd` (`docs/PLAN.md` section 8, "Its own service"): a status PGN to `127.0.0.1:8891` once a second; systemd `Type=notify`, `WatchdogSec`, `Restart=always`, no `Requires` either way | `aow-hatd` shows it as running or not and never waits on it |
| Who interprets the screens | The service. It flattens the object tree into a plain draw list | The browser needs no knowledge of ISOBUS, and the renderer can never be out of step with the stack |
| What the browser runs | Plain HTML, CSS and JavaScript: a canvas for the draw list, real input elements for typing. No native code, no plugin | The client is a browser, on any tablet. Only the picture and the touches cross Wi-Fi |
| Link to the browser | One WebSocket, straight from the browser to the service | No change to the app's host code. Moving it behind the app's socket later keeps the same messages |
| Web server | The repo's `aow_web` library (`web/server.cpp`) | Plain POSIX sockets, HTTP and WebSocket, no daemon headers in `web/server.h`. Check it links without the daemon before relying on it |
| Default | Off. Enabled per unit by the installer | A tractor with its own terminal would otherwise see two |
| Scheduling | Normal priority, every thread | It must never compete with the control loop |

## 3. What exists already

| Piece | Where | State |
|---|---|---|
| Terminal server: address claim, pool upload and parsing, responses to the implement, macros | AgIsoStack++ `isobus_virtual_terminal_server.cpp` (about 2,800 lines) and `isobus_virtual_terminal_objects.cpp` (about 8,600) | upstream, built by our CMake |
| A headless terminal on that class | `tests/isobus_vt.cpp` (191 lines) | works; the stand-in for `tests/isobus_vt_probe.py`. Prints strings and numbers, presses soft keys, enters values. Keeps no pools, draws nothing |
| A client that uploads a real pool | `aow-hatd`'s status screen (`daemon/isobus_vt.cpp`) | works against the stand-in on `vcan1` |
| Test pools | the stack's `examples/virtual_terminal/` (`VT3TestPool.iop`, aux functions, aux inputs) and `examples/seeder_example/BasePool.iop` | in the fetched source |
| A reference renderer | AgIsoVirtualTerminal (Open-Agriculture, GPL-3, desktop app on the same server class) | not in this repo. The drawing rules can be ported; the licence matches ours |
| A second reference renderer | espVT (`vt_scene_renderer.cpp`, about 3,000 lines, MIT; section 10) | not in this repo. About 15 object types, drawn on an LCD; MIT can be ported into GPL-3 |

What the server class gives the service to work with:

- **The object tree** of each connected implement (`get_object_tree()`), kept current as the
  implement changes values.
- **Events**: repaint, active mask changed, soft key mask changed, object focused. The repaint
  event is per implement, not per object, so the service finds what changed by comparing draw
  lists, as the stand-in already does for strings and numbers.
- **Input going back**: button and soft key press and release, numeric value, string value,
  input object selected.
- **Hooks the service must fill in**: screen and soft key sizes, the list of supported object
  types, and saving, loading and deleting stored pools.

## 4. Structure

```
implement ── CAN ── aow-vtd ── WebSocket ── browser (card in AgOpenWeb's web client)
                      │
                      ├─ stack thread: AgIsoStack++, answers the bus
                      ├─ flatten: object tree -> draw list, diff against the last one sent
                      ├─ web thread: page, WebSocket, status
                      └─ /var/lib/aow-vt/: stored pools, by implement NAME and version label
```

**Draw list.** For the active screen: rectangles, lines, ellipses, polygons, text, images and
tappable regions, each with the id of the object it came from. Positions are already resolved
(containers, pointers, hidden objects, enabled state), so the browser paints in order and
nothing else.

**Messages out.**

| Message | When |
|---|---|
| `screen` | a browser connects; the implement changes screen; another implement becomes active |
| `patch` | items changed, by id |
| `softkeys` | the soft key set changed |
| `image` | an image the browser has not had yet, by id. Sent once, then referred to by id |
| `alarm` | an alarm screen is raised or cleared, with its priority and sound |
| `implements` | the list of connected implements changed |

**Messages back.** `press` and `release` by id, `softkey`, `value` (a number or text for an
input field), `select` (focus an input), `activate` (choose which implement is shown),
`alarmAck`.

**Format.** JSON for everything but image pixels. Settle the image encoding in phase 1, after
measuring a real pool.

**Screen size.** The terminal tells each implement its screen size at connect, and the implement
scales its pool to fit. It is fixed for the session, so the service reports one size (480 x 480
to start) and the browser scales the canvas.

## 5. Rules

- **A press is always followed by a release.** If the WebSocket drops while a button is held,
  the service sends the release itself. An implement must never see a button stuck down because
  a tablet lost Wi-Fi.
- **One operator.** Any number of browsers may watch; one at a time may press. The service
  enforces it: the first to press holds control until idle for a set time or until it lets go.
- **The service never raises its own priority** and takes no lock that `aow-hatd`'s control
  thread uses. The two share only the CAN interface, and below it SPI0. Measured 2026-10-22
  (`docs/PLAN.md` section 4, "Loop timing under load"): CAN2 saturated at bus rate with every
  core busy at normal priority left the control loop's timing unchanged. Check `/api/status`
  `loop` again with a real pool upload in phase 0.
- **An unsupported object is drawn as a labelled box**, never dropped silently and never a
  reason to refuse the pool.
- **A bad pool is refused, not survived.** The stack reports the faulting object; the service
  logs it and tells the browser.
- **Alarms reach the operator with the card closed.** The status the app hears carries an
  "alarm active" flag (section 7).
- **Off means absent.** When disabled the service is not running and claims no address.
- **The stack: upstream first.** `third_party/` stays unmodified. If the server class lacks
  something, subclass it. Most of the gaps in section 10 are inside `process_rx_message` and
  `update()`, which a subclass cannot reach: send those upstream as pull requests. Until one is
  merged, `aow-vtd` may build the stack with a short patch list kept in this repo, one file per
  change, each naming its upstream pull request and the date. A patch goes when its pull request
  is merged and the pin moves past it.

## 6. Build order

Each phase leaves something that runs and can be checked on `vcan1`.

**Step −1: implement simulators (2026-10-22).** We own no ISOBUS implements, and open tools for
simulating them barely exist. A separate MIT repository, `isobus-implement-sim`
(<https://github.com/chriskinal/isobus-implement-sim>, private for now; plan in its
`docs/PLAN.md`), builds them: machines from profile files, driven by commands, with faults on
demand. Phases 1 to 5 below run against it, on `vcan1` and on CAN3 wired to CAN2. Phase 0 (a real
implement) stays, but moves after them: a simulator only proves we match our own reading of the
standard.

| Phase | What | Done when |
|---|---|---|
| 0 | **Bus trial, no code.** AgIsoVirtualTerminal on a Linux box against a real implement, on a direct CAN interface and then through the CAN-over-IP bridge | the implement's screens load and stay up for an hour on each link. If they do not load through the bridge, that route is closed before any code is written |
| 1 | **Dump.** Extend the stand-in to print the active screen as a draw list in JSON. Measure pool, screen and image sizes for the status screen, `VT3TestPool.iop` and the seeder pool | the dump for each pool matches what AgIsoVirtualTerminal shows; section 4's format is settled from the numbers |
| 2 | **`aow-vtd`, read-only.** Own executable and CMake target. Before the flattener reads the tree from another thread: the missing-object lookup and the working-set list lock (section 10, gaps 5 and 6). Macros on a mask's show and hide (gap 3). Page and WebSocket; `screen`, `patch`, `softkeys`, `image`. Text, numbers, rectangles, lines, ellipses, polygons, containers, pictures. A plain test page that draws it | `tests/vt_probe.py` on `vcan1`: connects as a browser, gets the daemon's status screen, sees a value change arrive as a patch, reconnects and gets the full screen again |
| 3 | **Input.** Buttons, soft keys, number, text, boolean and list inputs. The release-on-drop rule and the one-operator rule. Presses are sent from the web thread, so the transmit queue needs gap 4 fixed, or every send is handed to the stack thread | the probe presses a soft key and the client sees it; enters a value and the client receives it; kills its socket mid-press and the release is on the bus |
| 4 | **Stored pools.** Save, load and delete by version label under `/var/lib/aow-vt/`, keyed by the implement's full 64-bit NAME. Several implements at once, with `activate`. The restart test needs gaps 1 and 2 | restart the service: the client reconnects without uploading its pool again. Two clients on `vcan1`: either can be shown |
| 5 | **Alarms and the rest of the drawing.** Alarm masks with priority and sound (the audio commands need gap 7); meters and bar graphs; whatever phase 0's implements used that is still a labelled box | an alarm raised by a test client reaches the browser and the status flag; no labelled boxes on the phase 0 implements |
| 6 | **Packaging.** `aow-vtd.service`, installer option, in the `.aowpkg`, settings and a status line on the daemon's ISOBUS page | a fresh install with the option off has no terminal on the bus; with it on, the service starts at boot and survives an update |
| 7 | **AgOpenWeb card** (app repo, section 7) | a real implement is operated from a tablet for a working session |

Left out until someone needs them: auxiliary functions and inputs (joystick button assignment;
but both View-T and espVT treat it as a main feature, sections 9 and 10),
window masks and key groups (user-layout screens), graphics contexts, animations, external
object references, colour maps.

## 7. What AgOpenWeb needs

App code goes in the AgOpenWeb repo, not here. Its `Plans/ISOBUS_VT_FRONTEND.md` covers the
opposite role (the app's run screen on a tractor's terminal).

- **Knowing the terminal exists.** A small status message from the service to the app: terminal
  running, implement connected, alarm active. The task controller already reports through
  PGN 240; pick this one's number with the app.
- **The card.** Created by the web client while that status is heard. It opens the WebSocket to
  the service, paints the draw list on a canvas, and puts real input elements over fields that
  take typing.
- **Alarm with the card closed.** The app raises its own prompt and sound from the status flag.

Until phase 7 the service's own test page is the user interface, so phases 0 to 6 need no app
change.

## 8. Open items

1. **Identity on the bus.** A terminal claims an address with a manufacturer code. The stand-in
   uses 1407, as the stack's examples do. The same question is open for the task controller.
   espVT leaves the code and identity number as build settings; it has no answer either.
2. **Two terminals on one bus.** Allowed by the standard, but which one an implement picks
   varies by implement. Find out in phase 0 on a tractor that has its own terminal.
3. **Fonts.** The standard's fonts are fixed-size bitmap cells. A browser font scaled to the
   cell will not match exactly; decide in phase 2 whether that is good enough or the service
   ships a bitmap font. espVT draws every font size with one bundled monospace font (Roboto
   Mono), so one scaled font is apparently good enough in practice.
4. **The one-operator rule against the app's own.** Enforced in the service to start with. If
   the WebSocket later goes through the app, the app's rule replaces it.
5. **The WebSocket port.** `aow-hatd` holds 80, 8082, 8083, 23 and 2233. Proposed: 8084.
6. **The iOS and Android shells.** Whether they allow a second local WebSocket address as they
   stand has not been checked.
7. **Timing over the bridge.** Listening is forgiving; a pool upload is not. Phase 0 answers it.
8. **Who builds it.** Proposed as an outside contributors' project; the phases are sized so each
   is one reviewable change.

## 9. Prior art: SolSteer View-T

From the product page, <https://www.solsteer.com/view-t>, read 2026-10-13. Nothing here has been
tested by us.

**What it is.** A commercial tablet-as-terminal: an iOS or Android app, sold with a licence key,
that acts as a Universal Terminal, as the primary or a secondary one. The kit is a "CAN wireless
module", a replacement 9-way in-cab ISOBUS socket and a harness. The module is a Wi-Fi access
point that the tablet joins. The app also assigns AUX-N functions, records CAN traces and
updates SolSteer devices.

**Not stated on the page:** who makes the module, its protocol, or its transport. That it
carries CAN frames over TCP comes from the contributors who have one; a link to Proemion's
gateways is a guess from the protocol's name.

**How it differs from this plan.**

| | View-T | This plan |
|---|---|---|
| Where the terminal runs | in the tablet app | in `aow-vtd`, wired to the bus |
| What crosses Wi-Fi | every CAN frame, with the bus's timing | the picture and the touches |
| If Wi-Fi drops | the terminal leaves the bus | the terminal stays; the picture waits |
| Client | its own app, licensed | any browser, in AgOpenWeb's web client |

**What it tells us.**

- A pool upload and a terminal's timing survive one in-cab Wi-Fi hop to the module's own access
  point: a product ships that way. It says nothing about a longer or busier link, so phase 0
  still has to be run.
- The module is one way to reach an implement bus without a HAT: bridge it to a `vcan` and give
  `aow-vtd` that interface. The box running `aow-vtd` must then join the module's network, and
  the browser has to reach that box too. On a HAT none of this arises: `hatcan2` is wired.
- AUX-N assignment is a headline feature there. Section 6 leaves auxiliary functions out; if
  joystick assignment turns out to be why people want a tablet terminal, it earns its own phase.

## 10. Prior art: espVT

<https://github.com/MatNav1/espVT>, from a member of the ISOBUS group. Read 2026-10-22 from a
local copy at af8de3b. Nothing here has been tested by us.

**What it is.** A terminal server on a Waveshare ESP32-S3-Touch-LCD-7B (7 inch, 1024 x 600,
CAN through the ESP32's TWAI), MIT licence, marked experimental and not certified. It draws the
implement's screens on its own LCD and sends touches back; it also does AUX-N Type 2 (with a
separate ESP32 button panel), alarm sounds through an I2S amplifier, stored pools in flash,
GNSS (UM982 or RS-232) with NTRIP, and an RTK-fix gate for section control.

**The stack.** AgIsoStack++ at 509a920 (2026-07-21), changed in place in 14 files (their
`third_party/AgIsoStack-plus-plus/ESPVT_UPSTREAM.md`). Compared with upstream on 2026-10-22,
some of what they changed has since been fixed upstream: the Load Version response after a
stored pool is parsed (#766), choosing the working set to show (#773), the server's response
formats including a failed pool reported as a failure (#791), and 0xFF padding of short commands
(#792). These are still open in upstream 42ea80e (2026-10-19) and are what our phases will hit:

| # | Gap | espVT's change | Phase |
|---|---|---|---|
| 1 | After the terminal restarts, an implement carries on with maintenance messages without the init bit, and the server never takes it on | Accept it, and load its stored pool if there is exactly one version | 4 |
| 2 | Status messages start at once after a restart, so an implement never sees the terminal go away and does not restart its session | No status for the first 5 s | 4 |
| 3 | Changing the active data or soft key mask does not run the pool's On Show and On Hide macros | Run them, then On Change Active Mask | 2 |
| 4 | The transmit queue's `push` is safe for one producer only; touches, replies and periodic messages come from several threads | A mutex on `push`; the one consumer takes none | 3 |
| 5 | `get_object_by_id` uses `operator[]`, so a lookup of a missing id adds an empty entry to the tree | `find()` | 2 |
| 6 | The working-set list is read and changed from the CAN and update threads with no lock | A recursive mutex around it | 2 |
| 7 | Control Audio Signal and Set Audio Volume are acknowledged and dropped, with nothing to override | Two virtual functions | 5 |
| 8 | Every Change Numeric Value repaints, changed or not (upstream #796, open) | Repaint only on a change | 2 (our diff hides it, at a cost) |
| 9 | A long ETP stream during a pool upload is drained in one pass, starving the stack's other work | At most 32 frames or messages per pass | upload timing; check on the Pi |

Their changes are not ready to send as they are: some are ESP-only (`vTaskDelay`, PSRAM parser
stacks) and the logging is set for bring-up. Each gap above wants its own upstream pull request
with a test; upstream is merging VT server fixes within days.

**Other points.**

- **Screen size.** A choice of 480 x 480 (default, scaled to the panel) or 600 x 600 (1:1).
  Their note: an implement only fills 600 if it scales its own pool, so 480 is the safe default
  for unknown machines. It agrees with section 4.
- **Stored pools** are named by the low 32 bits of the implement's NAME and the version label.
  Ours use the full NAME (phase 4).
- **The renderer** covers about 15 object types in about 3,000 lines, with a touch map of the
  tappable areas: the same idea as our draw list, drawn on the device instead of in a browser.

