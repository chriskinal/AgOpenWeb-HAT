#!/usr/bin/env bash
# Installs the AOW HAT daemon. Asks once whether a HAT is fitted.
#
#   sudo ./install.sh            ask
#   sudo ./install.sh --hat      HAT fitted: install and enable the daemon
#   sudo ./install.sh --no-hat   no HAT (existing AiO board over Ethernet): leave it disabled
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF=/etc/aow-hat.conf
choice=""

for a in "$@"; do
  case "$a" in
    --hat) choice=yes ;;
    --no-hat) choice=no ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }

if [ -z "$choice" ]; then
  echo "Which hardware does this machine use?"
  echo "  1) AOW HAT fitted on the CM4 IO Board"
  echo "  2) An existing AiO board (v4.x / v5.x) connected by Ethernet"
  read -r -p "Enter 1 or 2: " n
  case "$n" in
    1) choice=yes ;;
    2) choice=no ;;
    *) echo "no choice made, nothing changed" >&2; exit 1 ;;
  esac
fi

echo "HAT_FITTED=$choice" > "$CONF"

if [ "$choice" = yes ]; then
  [ -x "$HERE/aow-hatd" ] || { echo "aow-hatd binary not found next to install.sh" >&2; exit 1; }
  install -m 0755 "$HERE/aow-hatd" /usr/local/bin/aow-hatd
  install -m 0644 "$HERE/aow-hatd.service" /etc/systemd/system/aow-hatd.service
  systemctl daemon-reload
  systemctl enable --now aow-hatd.service
  echo "HAT daemon installed and started."
else
  if systemctl list-unit-files aow-hatd.service >/dev/null 2>&1; then
    systemctl disable --now aow-hatd.service 2>/dev/null || true
  fi
  echo "No HAT: daemon left disabled. AgOpenWeb will use the AiO board on the network."
fi
echo "Choice saved in $CONF. Run this script again to change it."
