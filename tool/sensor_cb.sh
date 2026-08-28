#!/data/data/com.termux/files/usr/bin/bash
# termux-usb callback for the Perl sensor TUI. Recover + cold bring-up, then continuous SCAN streaming C/R/P
# lines to a FIFO the TUI reads directly (termux-usb holds a callback's stdout until it exits, so an infinite
# LOOP must stream via the FIFO, not stdout). CTL = channel-lock file, PING = "<mac> <ch> <tmpl>" TX request.
T=/root/ax56-ctl/tool
HOME=/root RECOVER=1 SCAN=1 LOOP=1 SCAN5=1 DWELL="${DWELL:-260}" \
CTL="$T/sensor.ctl" PING="$T/sensor.ping" \
BLOB="$T/full_ch6.bin" START=2329470 HB=1 \
FW="$T/fw_cut2_nic.bin" LOG="$T/sensor.chip.log" \
TXFD="$1" exec "$T/ax56ch" > "$T/sensor.fifo"
