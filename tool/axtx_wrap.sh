#!/data/data/com.termux/files/usr/bin/bash
HOME=/root \
BLOB=/root/ax56-ctl/tool/full_ch6.bin START=2329470 HB=1 \
FW=/root/ax56-ctl/tool/fw_cut2_nic.bin \
INITCAL=1 IQKCHAIN=1 TSSILIVE=1 DPKLIVE=1 \
INJECT=/root/ax56-ctl/tool/tx_mark.bin INJECT_REPS=300 \
LOG=/root/ax56-ctl/tool/axtx.log \
/root/.deno/bin/deno run -A --no-lock /root/ax56-ctl/tool/ax56tx.ts "$1"
