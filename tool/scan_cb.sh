#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: recover + cold bring-up, then SCAN the plan, dumping `R <hex>` EP0x84 transfers on stdout
# (chip chatter goes to the log). $1 = usbfs fd. Env: SCAN5=1 for 2.4+5 GHz, DWELL=<ms> per channel.
HOME=/root RECOVER=1 SCAN=1 SCAN5="${SCAN5:-}" DWELL="${DWELL:-450}" \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin \
LOG=/root/ax56-ctl/tool/scan.log \
TXFD="$1" exec /root/ax56-ctl/tool/ax56ch
