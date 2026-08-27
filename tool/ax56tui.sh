#!/data/data/com.termux/files/usr/bin/bash
# Launch the AX56 live TUI: find the adapter (mode-switch storage->wifi), then run ax56tui.mjs on it.
export PATH="/data/data/com.termux/files/usr/bin:$PATH"; cd /root/ax56-ctl/tool
find_wifi(){ for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "0b05" && { echo "$d"; return; }; done; }
DEV=$(find_wifi)
if [ -z "$DEV" ]; then
  S=$(for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "1a2b" && { echo "$d"; break; }; done)
  [ -n "$S" ] && { echo "switching adapter to Wi-Fi…"; bash ax56ctl.sh switch "$S" >/dev/null 2>&1; sleep 3; }
  DEV=$(find_wifi)
fi
[ -z "$DEV" ] && { echo "AX56 not found — plug it in and grant USB access."; exit 1; }
exec deno run --allow-read --allow-write --allow-run --allow-env ax56tui.mjs "$DEV" "$@"
