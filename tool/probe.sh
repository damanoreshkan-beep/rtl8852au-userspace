#!/data/data/com.termux/files/usr/bin/bash
exec deno run --allow-ffi --allow-read /root/ax56-ctl/tool/probe.ts "$1"
