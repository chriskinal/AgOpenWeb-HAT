#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
#
# Builds the AOW HAT update package. Run on Linux arm64 (the CM4, or an arm64 CI runner):
# needs cmake, g++, dtc and OpenSSL 3.
#
#   deploy/package.sh [--version X.Y.Z] [--key private.pem] [--out DIR] [--accepted-by public.pem]
#                     [--agopenweb agopenweb-linux-arm64.tar.gz]
#
# --agopenweb puts AgOpenWeb in the package too (a release does: the HAT software and the app as a
# matched set): its headless Linux arm64 tarball from an AgOpenWeb release, unchanged. install.sh
# installs it with its own installer when it differs from what is in /opt/agopenweb.
#
# Writes two files:
#   aow-hat-<version>-linux-arm64.tar.gz   the bundle: unpack and run "sudo ./install.sh --hat"
#                                          (first install on a new unit)
#   aow-hat-<version>-linux-arm64.aowpkg   the same bundle, signed, for the System Update page
#
# Package format: the tar.gz, then a 64-byte Ed25519 signature of it, then "AOWHAT1\n".
# The signing key comes from --key, $AOW_HAT_SIGNING_KEY, or
# ~/.config/aow-hat/release-signing-key.pem (CI: the AOW_HAT_SIGNING_KEY secret, release.yml). Its
# public half is deploy/update-key.pem, which the installer puts on the unit; a package signed with
# any other key is refused there, so the signature is checked against it before the package is
# written. --accepted-by checks it against another public key instead: a handover package for
# units that still trust an older key, signed with that key's private half, which installs the
# new deploy/update-key.pem on them.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
VERSION=""
KEY="${AOW_HAT_SIGNING_KEY:-$HOME/.config/aow-hat/release-signing-key.pem}"
OUT="$(pwd)"
ACCEPTED="$HERE/update-key.pem"
APP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --key) KEY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --accepted-by) ACCEPTED="$2"; shift 2 ;;
    --agopenweb) APP="$2"; shift 2 ;;
    -h|--help) sed -n '5,27p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ "$(uname -s)" = Linux ] || { echo "run on Linux (the daemon is Linux-only)" >&2; exit 1; }
[ -f "$KEY" ] || { echo "signing key not found: $KEY" >&2; exit 1; }
for tool in cmake dtc openssl; do command -v $tool >/dev/null || { echo "$tool not found" >&2; exit 1; }; done
[ -z "$APP" ] || [ -f "$APP" ] || { echo "AgOpenWeb tarball not found: $APP" >&2; exit 1; }

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
install -m 0755 "$HERE/install.sh" "$HERE/aow-hatctl" "$HERE/aow-keymon.py" "$HERE/aow-hat-update" "$HERE/aow-hat-app-tree" "$B/"
install -m 0644 "$HERE/aow-hatd.service" "$HERE/aow-linkd.service" "$HERE/aow-keymon.service" "$HERE/aow-hat-update.service" \
  "$HERE/80-aow-hat-can.rules" "$HERE/config.txt.hat" "$HERE/update-key.pem" "$B/"
install -m 0644 "$HERE"/can-config/*.json "$B/can-config/"
# The licences (LICENSE says which file is under which), and those of what is built in:
# AgIsoStack++ and ArduinoJson in aow-hatd, SharpIsoTerm's gateway as aow-linkd.
mkdir -p "$B/LICENSES"
install -m 0644 "$REPO/LICENSE" "$B/"
install -m 0644 "$REPO"/LICENSES/*.txt "$B/LICENSES/"
install -m 0644 "$REPO/third_party/ArduinoJson/LICENSE.txt" "$B/LICENSES/ArduinoJson.txt"
install -m 0644 "$REPO/third_party/aog-taskcontroller/README.md" "$B/LICENSES/AOG-TaskController.md"
[ -f "$BUILD/_deps/isobus-src/LICENSE" ] && install -m 0644 "$BUILD/_deps/isobus-src/LICENSE" "$B/LICENSES/AgIsoStack++.txt"
install -m 0644 "$BUILD/_deps/sharpisoterm-src/LICENSE" "$B/LICENSES/SharpIsoTerm.txt"
{
  echo "version=$VERSION"
  echo "arch=$(uname -m)"
  echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$B/VERSION"

# AgOpenWeb: checked to be the headless arm64 bundle with the virtual terminal in it, then carried
# as it came. VERSION gets its version as the app reports it and a hash of its files, which
# install.sh compares with /opt/agopenweb (both by aow-hat-app-tree).
if [ -n "$APP" ]; then
  A="$STAGE/agopenweb"
  mkdir -p "$A"
  tar -xzf "$APP" -C "$A"
  APPDIR="$A/agopenweb-linux-arm64"
  [ -x "$APPDIR/install.sh" ] && [ -f "$APPDIR/agopenweb.service" ] && [ -f "$APPDIR/app/AgOpenWeb.Desktop" ] \
    || { echo "$APP is not AgOpenWeb's headless Linux bundle (agopenweb-linux-arm64/{install.sh,app/})" >&2; exit 1; }
  [ "$(od -An -tx1 -j18 -N2 "$APPDIR/app/AgOpenWeb.Desktop" | tr -d ' ')" = b700 ] \
    || { echo "$APP is not for arm64" >&2; exit 1; }
  [ -f "$APPDIR/app/libisoshim.so" ] || { echo "$APP has no libisoshim.so: AgOpenWeb without its virtual terminal" >&2; exit 1; }
  APP_VERSION="$(grep -a -o -m1 -E '[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{7,}(-dirty)?' "$APPDIR/app/AgOpenWeb.Desktop.dll" || true)"
  APP_VERSION="${APP_VERSION%%$'\n'*}"  # the first, if the line had more
  [ -n "$APP_VERSION" ] || { echo "no version found in $APP's AgOpenWeb.Desktop.dll" >&2; exit 1; }
  install -m 0644 "$APP" "$B/agopenweb-linux-arm64.tar.gz"
  {
    echo "agopenweb=$APP_VERSION"
    echo "agopenweb_tree=$("$HERE/aow-hat-app-tree" "$APPDIR/app")"
  } >> "$B/VERSION"
fi

NAME="aow-hat-$VERSION-linux-arm64"
mkdir -p "$OUT"
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$B" -czf "$OUT/$NAME.tar.gz" .
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$OUT/$NAME.tar.gz" -out "$STAGE/sig"
[ "$(stat -c %s "$STAGE/sig")" = 64 ] || { echo "signature is not 64 bytes: is the key Ed25519?" >&2; exit 1; }
# The key must be the one units trust, or the package is useless.
openssl pkeyutl -verify -pubin -inkey "$ACCEPTED" -rawin -in "$OUT/$NAME.tar.gz" -sigfile "$STAGE/sig" >/dev/null \
  || { echo "signed, but not with the private half of $ACCEPTED" >&2; exit 1; }
{ cat "$OUT/$NAME.tar.gz" "$STAGE/sig"; printf 'AOWHAT1\n'; } > "$OUT/$NAME.aowpkg"

echo "$OUT/$NAME.tar.gz"
echo "$OUT/$NAME.aowpkg  (version $VERSION${APP_VERSION:+, AgOpenWeb $APP_VERSION}, $(stat -c %s "$OUT/$NAME.aowpkg") bytes)"
