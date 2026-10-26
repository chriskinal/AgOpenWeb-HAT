#!/usr/bin/env bash
# The end-to-end probes CI runs, against a build in BUILD (default build/). Runs as root in the
# Trixie container (.devcontainer/Dockerfile): the daemon's web pages are on port 80.
#
#   tests/ci_probes.sh          the probes on ptys and loopback: GNSS, IMU, serial ports, UM98x,
#                               then steering, the app link and the web pages against --board sim
#   tests/ci_probes.sh --can    the probes on virtual CAN: makes vcan0-vcan2 (needs the vcan module
#                               on the host and a privileged container), then CAN steering and
#                               GVRET, and the three ISOBUS probes
#
# Each probe's output is shown; the script stops at the first that fails.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
B="${BUILD:-build}"

probe() {
  echo "::group::$*"
  local rc=0
  timeout 300 python3 "$@" || rc=$?
  echo "::endgroup::"
  [ "$rc" -eq 0 ] || { echo "::error title=probe failed::$* (exit $rc)"; exit "$rc"; }
}

if [ "${1:-}" = --can ]; then
  for i in 0 1 2; do
    ip link show vcan$i >/dev/null 2>&1 || ip link add vcan$i type vcan
    ip link set vcan$i up
  done
  probe tests/can_probe.py "$B/aow-hatd"
  probe tests/isobus_probe.py "$B/aow-hatd"
  probe tests/isobus_tc_probe.py "$B/aow-hatd" "$B/isobus_implement"
  probe tests/isobus_vt_probe.py "$B/aow-hatd" "$B/isobus_vt" "$B/isobus_implement"
  exit 0
fi

probe tests/gnss_probe.py "$B/aow-hatd"
probe tests/imu_probe.py "$B/aow-hatd"
probe tests/ports_probe.py "$B/aow-hatd"
probe tests/um98x_probe.py "$B/aow-hatd"

# One simulated board for the rest: steering closes the loop on it; the app link and the web
# pages talk to it.
settings="$(mktemp -d)/settings.json"
"$B/aow-hatd" --board sim --config "$settings" > "$B/sim.log" 2>&1 &
sim=$!
trap 'kill $sim 2>/dev/null || true' EXIT
sleep 2
probe tests/steer_probe.py --settings "$settings"
probe tests/pgn_probe.py
probe tests/web_probe.py --port 80 --settings "$settings"
