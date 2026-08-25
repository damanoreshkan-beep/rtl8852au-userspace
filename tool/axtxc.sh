#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback: $1 = inherited usbfs fd. Chain params come via the exported environment.
TXFD="$1" exec /root/ax56-ctl/tool/ax56tx_c
