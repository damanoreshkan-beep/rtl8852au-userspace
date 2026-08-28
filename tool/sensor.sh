#!/data/data/com.termux/files/usr/bin/bash
# Sensor launcher: find the AX56 (switch it out of storage mode if needed), then run termux-usb streaming to the
# FIFO. sensor.pl spawns this in the background and reads the FIFO. Status (found device / no device) -> sensor.status.
export PATH="/data/data/com.termux/files/usr/bin:$PATH"
T=/root/ax56-ctl/tool; cd "$T" || exit 1
findwifi(){ for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "0b05" && { echo "$d"; return; }; done; }
DEV=$(findwifi)
if [ -z "$DEV" ]; then
  echo "SWITCH" > "$T/sensor.status"
  S=$(for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "1a2b" && { echo "$d"; break; }; done)
  [ -n "$S" ] && { bash ax56ctl.sh switch "$S" >/dev/null 2>&1; sleep 3; }
  DEV=$(findwifi)
fi
[ -z "$DEV" ] && { echo "NODEV" > "$T/sensor.status"; exit 1; }
echo "DEV $DEV" > "$T/sensor.status"
exec termux-usb -r -e "$T/sensor_cb.sh" "$DEV"
