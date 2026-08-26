#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: cold ch6 bring-up, then SETCH/SWEEP taken from the INHERITED env (export before termux-usb).
HOME=/root \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin \
LOG=/root/ax56-ctl/tool/chtest.log \
TXFD="$1" exec /root/ax56-ctl/tool/ax56ch
