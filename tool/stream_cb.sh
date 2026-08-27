#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: recover + cold bring-up, then CONTINUOUS SCAN streaming C/R to a FIFO the TUI reads
# directly — bypassing termux-usb, which holds a callback's stdout until it exits (an infinite LOOP never does).
HOME=/root RECOVER=1 SCAN=1 LOOP=1 SCAN5="${SCAN5:-}" DWELL="${DWELL:-300}" \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin LOG=/root/ax56-ctl/tool/stream.log \
TXFD="$1" exec /root/ax56-ctl/tool/ax56ch > /root/ax56-ctl/tool/ax56tui.fifo
