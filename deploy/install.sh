#!/usr/bin/env bash
# Installs the AOW HAT daemon. Asks once whether a HAT is fitted.
#
#   sudo ./install.sh            ask
#   sudo ./install.sh --hat      HAT fitted: install and enable the daemon
#   sudo ./install.sh --no-hat   no HAT (existing AiO board over Ethernet): leave it disabled
#
# With --hat, the AOW link gateway (aow-linkd, AgOpenWeb's virtual terminal on the ISOBUS
# channel) is installed too, and left as it was unless one of these is given:
#   --gateway      enable and start it
#   --no-gateway   stop and disable it
#
# Run from a bundle (deploy/package.sh) or from the repo's deploy/ with a built aow-hatd beside
# it. From a bundle, the bundle is kept in /usr/local/lib/aow-hat/current and the one before it
# in .../previous, which is what the updater rolls back to. --no-record skips that (the
# updater's own rollback uses it).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF=/etc/aow-hat.conf
LIB=/usr/local/lib/aow-hat
choice=""
record=yes
gateway=""

for a in "$@"; do
  case "$a" in
    --hat) choice=yes ;;
    --no-hat) choice=no ;;
    --no-record) record=no ;;
    --gateway) gateway=yes ;;
    --no-gateway) gateway=no ;;
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
  [ -x "$HERE/aow-linkd" ] || { echo "aow-linkd binary not found next to install.sh" >&2; exit 1; }
  install -m 0755 "$HERE/aow-linkd" /usr/local/bin/aow-linkd
  install -m 0644 "$HERE/aow-linkd.service" /etc/systemd/system/aow-linkd.service
  install -m 0644 "$HERE/aow-hatd.service" /etc/systemd/system/aow-hatd.service
  install -m 0755 "$HERE/aow-keymon.py" /usr/local/bin/aow-keymon.py
  install -m 0644 "$HERE/aow-keymon.service" /etc/systemd/system/aow-keymon.service
  # The updater behind the System Update page, and the key update packages must be signed with.
  install -d "$LIB" /var/lib/aow-hat/update
  install -m 0755 "$HERE/aow-hat-update" "$LIB/aow-hat-update"
  install -m 0644 "$HERE/update-key.pem" "$LIB/update-key.pem"
  install -m 0644 "$HERE/aow-hat-update.service" /etc/systemd/system/aow-hat-update.service
  # The firmware's CAN brand files, for the CAN upload page.
  install -d /usr/share/aow-hat/can-config
  install -m 0644 "$HERE"/can-config/*.json /usr/share/aow-hat/can-config/
  install -d /etc/aow-hat
  # Options that belong to this unit, not to the software: never overwritten by an update.
  if [ ! -f /etc/aow-hat/daemon.env ]; then
    cat > /etc/aow-hat/daemon.env <<'ENV'
# Extra aow-hatd options for this unit. Kept across updates.
# --adc-ref: the measured 5 V on J3 pin 3 (5V_MAIN), the ADC reference.
AOW_HATD_OPTS=--adc-ref 5.0
ENV
  fi
  if [ ! -f /etc/aow-hat/linkd.env ]; then
    cat > /etc/aow-hat/linkd.env <<'ENV'
# Extra aow-linkd options for this unit. Kept across updates.
# It listens on 127.0.0.1 (AgOpenWeb on this unit) unless told otherwise. "--listen 0.0.0.0" lets
# a tablet on the unit's network reach it: anyone who can reach port 2323 can then put frames on
# the implement's bus, so only on a network that is the user's own.
# "--can IF" if the ISOBUS channel is not CAN2 (hatcan2).
AOW_LINKD_OPTS=
ENV
  fi

  # Boot configuration for the HAT: overlay, config.txt block, CAN interface names.
  # The overlay is compiled here when dtc is present, else aow-hat.dtbo next to this script.
  reboot_needed=no
  dts="$HERE/../boards/hat/aow-hat-overlay.dts"
  dtbo="$(mktemp)"
  if [ -f "$HERE/aow-hat.dtbo" ]; then
    cp "$HERE/aow-hat.dtbo" "$dtbo"
  elif command -v dtc >/dev/null && [ -f "$dts" ]; then
    dtc -@ -q -I dts -O dtb -o "$dtbo" "$dts"
  else
    echo "no aow-hat.dtbo next to install.sh and no dtc to build it (apt install device-tree-compiler)" >&2; exit 1
  fi
  if ! cmp -s "$dtbo" /boot/firmware/overlays/aow-hat.dtbo; then
    install -m 0644 "$dtbo" /boot/firmware/overlays/aow-hat.dtbo
    reboot_needed=yes
  fi
  rm -f "$dtbo"
  install -m 0644 "$HERE/80-aow-hat-can.rules" /etc/udev/rules.d/80-aow-hat-can.rules
  # The HAT's block in config.txt: added the first time, replaced when a new version changes it.
  cfg=/boot/firmware/config.txt
  if [ "$(sed -n '/^# --- AOW HAT/,/^# --- end AOW HAT/p' "$cfg")" != "$(cat "$HERE/config.txt.hat")" ]; then
    [ -f "$cfg.pre-hat" ] || cp "$cfg" "$cfg.pre-hat"
    tmp="$(mktemp)"
    sed '/^# --- AOW HAT/,/^# --- end AOW HAT/d' "$cfg" > "$tmp"
    cat "$HERE/config.txt.hat" >> "$tmp"
    cat "$tmp" > "$cfg"
    rm -f "$tmp"
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
  case "$gateway" in
    yes) systemctl enable aow-linkd.service; systemctl restart aow-linkd.service; echo "AOW link gateway enabled and started." ;;
    no) systemctl disable --now aow-linkd.service 2>/dev/null || true; echo "AOW link gateway disabled." ;;
    *) systemctl try-restart aow-linkd.service ;;  # running: run the new binary
  esac
  if [ "$reboot_needed" = yes ]; then
    touch /run/aow-hat-reboot-needed
    echo "Boot files changed: reboot to load the HAT overlays and the RT kernel."
  fi
  # Keep this bundle as "current" and the one before as "previous" (the updater's rollback).
  if [ "$record" = yes ] && [ -f "$HERE/VERSION" ] && [ "$HERE" != "$LIB/current" ]; then
    rm -rf "$LIB/previous"
    [ -d "$LIB/current" ] && mv "$LIB/current" "$LIB/previous"
    cp -a "$HERE" "$LIB/current"
  fi
else
  if systemctl list-unit-files aow-hatd.service >/dev/null 2>&1; then
    systemctl disable --now aow-hatd.service 2>/dev/null || true
  fi
  if systemctl list-unit-files aow-linkd.service >/dev/null 2>&1; then
    systemctl disable --now aow-linkd.service 2>/dev/null || true
  fi
  echo "No HAT: daemon left disabled. AgOpenWeb will use the AiO board on the network."
fi
echo "Choice saved in $CONF. Run this script again to change it."
