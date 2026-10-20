#!/usr/bin/env bash
# Build, test and package aow-hatd on the Mac, in a Debian Trixie arm64 container (the CM4's OS
# and architecture), and install a package on a unit through its System Update page.
#
#   deploy/dev.sh build                      configure and build (incremental)
#   deploy/dev.sh test                       build, then ctest
#   deploy/dev.sh package --version X.Y.Z    signed .aowpkg and .tar.gz into dist/
#   deploy/dev.sh install FILE [HOST]        upload a .aowpkg to HOST's update page and follow it
#   deploy/dev.sh shell                      a shell in the container
#
# Needs Docker (OrbStack or Docker Desktop) on an arm64 host. Builds live in the Docker volume
# aow-hat-build, not in the source tree, so they survive between runs and stay off the Mac's
# file sharing. The signing key is mounted read-only from ~/.config/aow-hat/. HOST defaults to
# $AOW_HAT_HOST, else the dev CM4. CMake options go after the command: dev.sh build -DAOW_ISOBUS=OFF
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=aow-hat-dev
VOLUME=aow-hat-build
KEY="${AOW_HAT_SIGNING_KEY:-$HOME/.config/aow-hat/update-signing-key.pem}"
HOST="${AOW_HAT_HOST:-192.168.4.217}"

image() {
  docker build -q -t "$IMAGE" "$REPO/.devcontainer" >/dev/null
}

# Runs a command in the container: source at /src, builds in /build.
run() {
  local tty=()
  [ -t 0 ] && [ -t 1 ] && tty=(-it)
  docker run --rm ${tty[@]+"${tty[@]}"} -v "$REPO":/src -v "$VOLUME":/build ${EXTRA[@]+"${EXTRA[@]}"} "$IMAGE" "$@"
}

configure_and_build() {
  run bash -c 'cmake -S /src -B /build/main -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" >/dev/null &&
               cmake --build /build/main -j"$(nproc)"' _ "$@"
}

cmd="${1:-}"
[ -n "$cmd" ] && shift
EXTRA=()
case "$cmd" in
  build)
    image
    configure_and_build "$@"
    ;;
  test)
    image
    configure_and_build "$@"
    run ctest --test-dir /build/main --output-on-failure
    ;;
  package)
    [ -f "$KEY" ] || { echo "signing key not found: $KEY" >&2; exit 1; }
    image
    mkdir -p "$REPO/dist"
    EXTRA=(-v "$KEY":/key.pem:ro -e AOW_PACKAGE_BUILD=/build/package)
    run /src/deploy/package.sh --key /key.pem --out /src/dist "$@"
    ;;
  install)
    f="${1:?usage: dev.sh install FILE.aowpkg [HOST]}"
    host="${2:-$HOST}"
    [ -f "$f" ] || { echo "no such file: $f" >&2; exit 1; }
    echo "== $(basename "$f") to $host"
    curl -sS -m 60 -w " (http %{http_code})\n" -H "Content-Type: application/octet-stream" \
      --data-binary @"$f" "http://$host/api/ota/upload"
    last="" st=""
    for _ in $(seq 1 90); do
      s=$(curl -s -m 2 "http://$host/api/ota/status") || { sleep 1; continue; }
      st=$(sed -n 's/.*"state":"\([a-z_]*\)".*/\1/p' <<<"$s")
      msg=$(sed -n 's/.*"message":"\([^"]*\)".*/\1/p' <<<"$s")
      [ "$s" != "$last" ] && { last="$s"; echo "  $st: $msg"; }
      case "$st" in done|failed|rolled_back) break ;; esac
      sleep 1
    done
    echo "  running: $(curl -s -m 3 "http://$host/api/status" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p')"
    [ "$st" = done ]
    ;;
  shell)
    image
    run bash
    ;;
  *)
    sed -n '2,16p' "$0"
    exit 2
    ;;
esac
