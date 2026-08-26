#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: RECOVER a warm-dirty chip (pwroff+pwron) then the normal cold bring-up + SETCH/RCK.
HOME=/root RECOVER=1 \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin \
LOG=/root/ax56-ctl/tool/recover.log \
TXFD="$1" exec /root/ax56-ctl/tool/ax56ch
