#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: $1 = inherited usbfs fd. Cold monitor-RX bring-up on ch6 (proven path) + RF readback.
HOME=/root \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin \
WRFTEST=1 RXPROBE=1 \
LOG=/root/ax56-ctl/tool/rxtest.log \
TXFD="$1" exec /root/ax56-ctl/tool/ax56tx_c
