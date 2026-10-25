#!/usr/bin/env bash
# Builds the AOW HAT update package. Run on Linux arm64 (the CM4, or an arm64 CI runner):
# needs cmake, g++, dtc and OpenSSL 3.
#
#   deploy/package.sh [--version X.Y.Z] [--key private.pem] [--out DIR]
#
# Writes two files:
#   aow-hat-<version>-linux-arm64.tar.gz   the bundle: unpack and run "sudo ./install.sh --hat"
#                                          (first install on a new unit)
#   aow-hat-<version>-linux-arm64.aowpkg   the same bundle, signed, for the System Update page
#
# Package format: the tar.gz, then a 64-byte Ed25519 signature of it, then "AOWHAT1\n".
# The signing key comes from --key, $AOW_HAT_SIGNING_KEY, or
# ~/.config/aow-hat/update-signing-key.pem. Its public half is deploy/update-key.pem, which the
# installer puts on the unit; a package signed with any other key is refused there.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
VERSION=""
KEY="${AOW_HAT_SIGNING_KEY:-$HOME/.config/aow-hat/update-signing-key.pem}"
OUT="$(pwd)"
while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --key) KEY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ "$(uname -s)" = Linux ] || { echo "run on Linux (the daemon is Linux-only)" >&2; exit 1; }
[ -f "$KEY" ] || { echo "signing key not found: $KEY" >&2; exit 1; }
for tool in cmake dtc openssl; do command -v $tool >/dev/null || { echo "$tool not found" >&2; exit 1; }; done

BUILD="${AOW_PACKAGE_BUILD:-$REPO/build-package}"  # deploy/dev.sh puts it in its build volume
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release ${VERSION:+-DAOW_VERSION="$VERSION"} >/dev/null
cmake --build "$BUILD" --target aow-hatd aow-linkd -j"$(nproc)" >/dev/null
VERSION="$("$BUILD/aow-hatd" --version | awk '{print $2}')"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
B="$STAGE/bundle"
mkdir -p "$B/can-config"
install -m 0755 "$BUILD/aow-hatd" "$B/aow-hatd"
strip "$B/aow-hatd" 2>/dev/null || true
install -m 0755 "$BUILD/aow-linkd" "$B/aow-linkd"
strip "$B/aow-linkd" 2>/dev/null || true
dtc -@ -q -I dts -O dtb -o "$B/aow-hat.dtbo" "$REPO/boards/hat/aow-hat-overlay.dts"
install -m 0755 "$HERE/install.sh" "$HERE/aow-hatctl" "$HERE/aow-keymon.py" "$HERE/aow-hat-update" "$B/"
install -m 0644 "$HERE/aow-hatd.service" "$HERE/aow-linkd.service" "$HERE/aow-keymon.service" "$HERE/aow-hat-update.service" \
  "$HERE/80-aow-hat-can.rules" "$HERE/config.txt.hat" "$HERE/update-key.pem" "$B/"
install -m 0644 "$HERE"/can-config/*.json "$B/can-config/"
{
  echo "version=$VERSION"
  echo "arch=$(uname -m)"
  echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$B/VERSION"

NAME="aow-hat-$VERSION-linux-arm64"
mkdir -p "$OUT"
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$B" -czf "$OUT/$NAME.tar.gz" .
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$OUT/$NAME.tar.gz" -out "$STAGE/sig"
[ "$(stat -c %s "$STAGE/sig")" = 64 ] || { echo "signature is not 64 bytes: is the key Ed25519?" >&2; exit 1; }
# The key must be the one units trust, or the package is useless.
openssl pkeyutl -verify -pubin -inkey "$HERE/update-key.pem" -rawin -in "$OUT/$NAME.tar.gz" -sigfile "$STAGE/sig" >/dev/null \
  || { echo "signed, but not with the key in deploy/update-key.pem" >&2; exit 1; }
{ cat "$OUT/$NAME.tar.gz" "$STAGE/sig"; printf 'AOWHAT1\n'; } > "$OUT/$NAME.aowpkg"

echo "$OUT/$NAME.tar.gz"
echo "$OUT/$NAME.aowpkg  (version $VERSION, $(stat -c %s "$OUT/$NAME.aowpkg") bytes)"
