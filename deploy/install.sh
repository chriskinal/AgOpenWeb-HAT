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
  install -m 0755 "$HERE/aow-hatctl" /usr/local/bin/aow-hatctl
  install -m 0644 "$HERE/aow-hatd.service" /etc/systemd/system/aow-hatd.service
  install -m 0755 "$HERE/aow-keymon.py" /usr/local/bin/aow-keymon.py
  install -m 0644 "$HERE/aow-keymon.service" /etc/systemd/system/aow-keymon.service
  # The firmware's CAN brand files, for the CAN upload page.
  install -d /usr/share/aow-hat/can-config
  install -m 0644 "$HERE"/can-config/*.json /usr/share/aow-hat/can-config/
  install -d /etc/aow-hat

  # Boot configuration for the HAT: overlay, config.txt block, CAN interface names.
  # The overlay is compiled here when dtc is present, else aow-hat.dtbo next to this script.
  dts="$HERE/../boards/hat/aow-hat-overlay.dts"
  if command -v dtc >/dev/null && [ -f "$dts" ]; then
    dtc -@ -q -I dts -O dtb -o /boot/firmware/overlays/aow-hat.dtbo "$dts"
  elif [ -f "$HERE/aow-hat.dtbo" ]; then
    install -m 0644 "$HERE/aow-hat.dtbo" /boot/firmware/overlays/aow-hat.dtbo
  else
    echo "no dtc and no aow-hat.dtbo next to install.sh (apt install device-tree-compiler)" >&2; exit 1
  fi
  install -m 0644 "$HERE/80-aow-hat-can.rules" /etc/udev/rules.d/80-aow-hat-can.rules
  reboot_needed=no
  cfg=/boot/firmware/config.txt
  if ! grep -q '^# --- AOW HAT' "$cfg"; then
    cp "$cfg" "$cfg.pre-hat"
    cat "$HERE/config.txt.hat" >> "$cfg"
    reboot_needed=yes
  fi
  # The PREEMPT_RT kernel (apt install linux-image-rpi-v8-rt), if it is there and not selected.
  if [ -f /boot/firmware/kernel8_rt.img ] && ! grep -q '^kernel=kernel8_rt.img' "$cfg"; then
    echo "kernel=kernel8_rt.img" >> "$cfg"
    reboot_needed=yes
  fi
  systemctl disable hciuart 2>/dev/null || true
  systemctl daemon-reload
  systemctl enable aow-hatd.service aow-keymon.service
  systemctl restart aow-keymon.service
  systemctl restart aow-hatd.service  # a reinstall must run the new binary
  echo "HAT daemon installed and started."
  [ "$reboot_needed" = yes ] && echo "config.txt changed: reboot to load the HAT overlays and the RT kernel."
else
  if systemctl list-unit-files aow-hatd.service >/dev/null 2>&1; then
    systemctl disable --now aow-hatd.service 2>/dev/null || true
  fi
  echo "No HAT: daemon left disabled. AgOpenWeb will use the AiO board on the network."
fi
echo "Choice saved in $CONF. Run this script again to change it."
