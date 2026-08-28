#!/bin/bash
# x56 entry (symlink /usr/local/bin/x56 -> this file). Dispatch:
#   x56            -> launch the sensor touch-TUI (live; needs the adapter + USB popup)
#   x56 demo       -> sensor TUI in demo mode (synthetic activity, no adapter needed)
#   x56 build      -> full build + survey + reachability-probe experiment (log -> ax56build.log, prints OK/ERROR)
#   x56 box | tx   -> bench-box monitor / sustained TX (two-radio tests)
set -u
export PATH="/data/data/com.termux/files/usr/bin:$PATH"
T=/root/ax56-ctl/tool
cd "$T" 2>/dev/null || { echo ERROR; exit 1; }

# sensor (default) / demo -> interactive Perl touch-TUI; owns the terminal, builds ax56ch if stale. Tap USB popup.
CMD="${1:-sensor}"
if [ "$CMD" = "demo" ]; then exec perl "$T/sensor.pl" demo; fi   # demo is pure Perl, needs no driver
if [ "$CMD" = "sensor" ]; then
  if [ ! -x "$T/ax56ch" ] || [ "$T/hwdriver.c" -nt "$T/ax56ch" ]; then
    gcc -O2 -o "$T/ax56ch" "$T/hwdriver.c" $(pkg-config --cflags --libs libusb-1.0) || { echo "build failed"; exit 1; }
  fi
  exec perl "$T/sensor.pl"
fi

L="$T/ax56build.log"; : > "$L"
lg(){ echo "$@" >> "$L"; }
COMMON="HOME=/root RECOVER=1 BLOB=$T/full_ch6.bin START=2329470 HB=1 FW=$T/fw_cut2_nic.bin LOG=$T/build.chip.log"
CB="$T/.build_cb.sh"

# box mode: put the bench box's Intel wifi in monitor on ch6 and capture frames from the AX56's TA. Uses the
# safe heredoc + S() sudo pattern on the box (never nested quoting). Output -> ax56box.log for Claude to read.
if [ "${1:-}" = "box" ]; then
  BL="$T/ax56box.log"; : > "$BL"
  /usr/bin/ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 mrx@192.168.50.114 'bash -s' >> "$BL" 2>&1 <<'SSH'
S(){ echo 1 | sudo -S "$@" 2>/dev/null; }
IF=wlp3s0
S systemctl stop NetworkManager 2>/dev/null; S systemctl stop wpa_supplicant 2>/dev/null; S pkill wpa_supplicant 2>/dev/null
S ip link set "$IF" down; S iw dev "$IF" set monitor control; S ip link set "$IF" up; S iw dev "$IF" set channel 6
echo "iface: $(iw dev "$IF" info 2>/dev/null | awk '/type/{print $2} /channel/{print "ch"$2}' | tr '\n' ' ')"
echo "capturing 70s to pcap ..."
S timeout 70 tcpdump -i "$IF" -e -n -w /tmp/ax56.pcap 2>/dev/null
echo "TOTAL frames captured: $(S tcpdump -r /tmp/ax56.pcap 2>/dev/null | wc -l)"
echo "FROM AX56 TA 02:a5:56:00:00:01: $(S tcpdump -r /tmp/ax56.pcap -n 'wlan addr2 02:a5:56:00:00:01' 2>/dev/null | wc -l)"
S tcpdump -r /tmp/ax56.pcap -e -n 'wlan addr2 02:a5:56:00:00:01' 2>/dev/null | head -4
echo "capture done"
SSH
  echo OK; exit 0
fi

lg "== build =="
if ! gcc -O2 -o ax56ch hwdriver.c $(pkg-config --cflags --libs libusb-1.0) 2>>"$L"; then lg "BUILD FAILED"; echo ERROR; exit 1; fi
lg "build OK"

# run one termux-usb pass with the given extra env; stdout -> $1 file
pass(){ local out="$1"; shift
  { echo '#!/data/data/com.termux/files/usr/bin/bash'; echo "$COMMON $* TXFD=\"\$1\" exec $T/ax56ch"; } > "$CB"; chmod +x "$CB"
  termux-usb -r -e "$CB" "$DEV" > "$out" 2>>"$L"
}
kill_stragglers(){ for p in $(ps aux 2>/dev/null | grep -iE "[a]x56ch" | awk '{print $2}'); do kill "$p" 2>/dev/null; done; }

kill_stragglers; sleep 0.3
findwifi(){ for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "0b05" && { echo "$d"; return; }; done; }
DEV=$(findwifi)
if [ -z "$DEV" ]; then
  S=$(for d in $(termux-usb -l 2>/dev/null | grep -oE "/dev/bus/usb/[0-9]+/[0-9]+"); do bash ax56ctl.sh id "$d" 2>/dev/null | grep -q "1a2b" && { echo "$d"; break; }; done)
  [ -n "$S" ] && { lg "switching to wifi mode"; bash ax56ctl.sh switch "$S" >>"$L" 2>&1; sleep 3; }
  DEV=$(findwifi)
fi
if [ -z "$DEV" ]; then lg "no adapter found"; echo ERROR; exit 1; fi
lg "adapter: $DEV"

# tx mode: sustained transmit on ch6 (TA 02:a5:56:00:00:01) so a 2nd monitor (the box) can catch the radiation
if [ "${1:-}" = "tx" ]; then
  lg "== TX burst on ch6, TA 02:a5:56:00:00:01, ~10s — run the box monitor NOW =="
  pass "$T/.build.tx" "TXREPS=8000 PINGTEST=\"14:cc:20:33:23:88 6\""
  grep -E "^P " "$T/.build.tx" >> "$L"
  grep -iE "0xF0=0x|path_ok|BOOTED" "$T/build.chip.log" 2>/dev/null | tail -3 >> "$L"
  kill_stragglers; echo OK; exit 0
fi

# 1) survey -> table -> strongest AP (MAC + channel)
lg "== survey =="
pass "$T/.build.scan" SCAN=1 SCAN5=1 DWELL=450
deno run --allow-read scan_table.mjs < "$T/.build.scan" 2>>"$L" | sed 's/\x1b\[[0-9;]*m//g' > "$T/.table.txt"
cat "$T/.table.txt" >> "$L"
# prefer a ch6 AP (the bring-up channel — no retune, RX stays armed); else the strongest AP
read -r MAC CH < <(awk '$4 ~ /^[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:/ && $2==6 {print $4, $2; exit}' "$T/.table.txt")
[ -z "${MAC:-}" ] && read -r MAC CH < <(awk '$4 ~ /^[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:/ {print $4, $2; exit}' "$T/.table.txt")
if [ -z "${MAC:-}" ] || [ -z "${CH:-}" ]; then lg "no AP found to probe"; echo OK; exit 0; fi
lg "target AP: $MAC ch$CH"

# 2) reachability probe WITH calibration
kill_stragglers
lg "== probe WITH cal =="
pass "$T/.build.p1" "PINGTEST=\"$MAC $CH\""
grep -E "^P " "$T/.build.p1" >> "$L" || lg "(no result line)"
grep -iE "RXPROBE|0xF0=0x|BOOTED|entry 0x1e0|WEDGED|path_ok" "$T/build.chip.log" 2>/dev/null | tail -6 >> "$L"

# 3) reachability probe WITHOUT calibration (baseline: is RX alive, does it radiate anyway)
kill_stragglers
lg "== probe NO cal =="
pass "$T/.build.p2" "PINGNOCAL=1 PINGTEST=\"$MAC $CH\""
grep -E "^P " "$T/.build.p2" >> "$L" || lg "(no result line)"
grep -iE "ce20:|RXPROBE|0xF0=0x|BOOTED|entry 0x1e0|WEDGED" "$T/build.chip.log" 2>/dev/null | tail -4 >> "$L"

kill_stragglers
echo OK
