#!/data/data/com.termux/files/usr/bin/bash
# ax56ctl — thin front-end over termux-usb for the RTL8852AU control core.
# Usage:
#   ax56ctl list
#   ax56ctl id      <dev>
#   ax56ctl switch  <dev>                 # SCSI eject: storage -> wifi
#   ax56ctl reg     <dev> <hexAddr> [count]
#   ax56ctl regw    <dev> <hexAddr> <hexVal>
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DENO="${DENO:-/root/.deno/bin/deno}"
CORE="$HERE/ax56.ts"
export PATH="/data/data/com.termux/files/usr/bin:$PATH"

cb="$HERE/.cb.sh"
cat > "$cb" <<EOF
#!/data/data/com.termux/files/usr/bin/bash
HOME=/root "$DENO" run -A --no-lock "$CORE" "\$1"
EOF
chmod +x "$cb"

run() { # dev
  local dev="$1"; shift
  timeout 60 termux-usb -r -e "$cb" "$dev"
}

cmd="${1:-list}"; shift || true
case "$cmd" in
  list)   termux-usb -l ;;
  id)     AX56_ACTION=id                                  run "$1" ;;
  switch) AX56_ACTION=switch                              run "$1" ;;
  reg)    AX56_ACTION=reg  AX56_ADDR=$((16#${2:-0})) AX56_COUNT="${3:-16}" run "$1" ;;
  regw)   AX56_ACTION=regw AX56_ADDR=$((16#$2)) AX56_VAL=$((16#$3))        run "$1" ;;
  *)      echo "unknown: $cmd"; exit 2 ;;
esac
