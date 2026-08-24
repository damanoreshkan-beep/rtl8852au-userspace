#!/usr/bin/env -S deno run --allow-read --allow-write
// parse-usbmon.ts — turn a usbmon capture of the kernel rtw89 bringing the AX56 up in monitor mode on some
// channel into a replay op-blob for hwdriver.c, and print the last fwdl cycle's post-fwdl tail offset.
//
// Capture (on a host with the kernel rtw89 driver + usbmon):
//   sudo sh -c 'tcpdump -i usbmon1 -w full_ch1.pcap -s0 & \
//     modprobe -i rtw89_core; modprobe -i rtw89_usb; modprobe -i rtw89_8852a; modprobe -i rtw89_8852au; \
//     sleep 4; IF=$(ls /sys/class/net | grep wlp0s20 | head -1); \
//     ip link set $IF down; iw dev $IF set type monitor; ip link set $IF up; iw dev $IF set channel 1; \
//     sleep 2; kill %1'
// Parse:  deno run --allow-read --allow-write parse-usbmon.ts full_ch1.pcap full_ch1.bin
// Replay: sudo ./hwdriver full_ch1.bin <printed-tail-offset> hb   (then read beacons off EP 0x84)
//
// The op-blob format matches hwdriver.c: kind1 ctrl write [1][0x40][0x05][wv:2][wi:2][ln:2][data], kind3 read
// [3][wv:2][wi:2], kind4 poll [4][wv:2][wi:2][val:4], kind2 bulk [2][ep][ln:2][data]. Only the AX56's OUT
// traffic is emitted; bulk-IN (RX) is left for hwdriver to read live.
const [inPath, outPath] = [Deno.args[0] ?? "full_ch1.pcap", Deno.args[1] ?? "full_ch1.bin"];
const buf = Deno.readFileSync(inPath);
const u16 = (p: number) => buf[p] | (buf[p + 1] << 8);
const u32 = (p: number) => (buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16) | (buf[p + 3] << 24)) >>> 0;
const idKey = (p: number) => { let s = ""; for (let i = 0; i < 8; i++) s += buf[p + i].toString(16).padStart(2, "0"); return s; };
if (u32(20) !== 220) console.warn(`warning: DLT is ${u32(20)}, expected 220 (DLT_USB_LINUX_MMAPPED)`);

// pass 0: pick the AX56 device (most 0x40/0x05 vendor writes) + map URB id -> read return value
const wcnt: Record<number, number> = {};
const ret = new Map<string, number>();
let p = 24;
while (p + 16 <= buf.length) {
  const incl = u32(p + 8), rec = p + 16; if (rec + incl > buf.length) break;
  const h = rec, type = buf[h + 8], xfer = buf[h + 9], dev = buf[h + 11], setup = h + 40, lenCap = u32(h + 36);
  if (type === 0x53 && xfer === 2 && buf[setup] === 0x40 && buf[setup + 1] === 0x05) wcnt[dev] = (wcnt[dev] || 0) + 1;
  if (type === 0x43 && xfer === 2 && lenCap >= 4) ret.set(idKey(h), u32(rec + 64));
  p = rec + incl;
}
const devnum = Number(Object.entries(wcnt).sort((a, b) => b[1] - a[1])[0][0]);

// pass 1: emit ops in order (reads: a spin of >= POLL_MIN same-addr reads collapses to one kind4 poll)
const POLL_MIN = 8;
const out: number[] = [];
let nw = 0, nr = 0, npoll = 0, nb = 0, runAddr = -1, runCount = 0, runVal = 0;
const emitRead = (a: number) => { out.push(3, a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, (a >> 24) & 0xff); nr++; };
const flush = () => {
  if (runCount === 0) return;
  if (runCount >= POLL_MIN) {
    out.push(4, runAddr & 0xff, (runAddr >> 8) & 0xff, (runAddr >> 16) & 0xff, (runAddr >> 24) & 0xff,
      runVal & 0xff, (runVal >> 8) & 0xff, (runVal >> 16) & 0xff, (runVal >>> 24) & 0xff); npoll++;
  } else for (let i = 0; i < runCount; i++) emitRead(runAddr);
  runCount = 0; runAddr = -1;
};
p = 24;
while (p + 16 <= buf.length) {
  const incl = u32(p + 8), rec = p + 16; if (rec + incl > buf.length) break;
  const h = rec, type = buf[h + 8], xfer = buf[h + 9], dev = buf[h + 11], epnum = buf[h + 10], setup = h + 40;
  if (dev === devnum && type === 0x53) {
    if (xfer === 2 && buf[setup + 1] === 0x05 && buf[setup] === 0x40) {
      flush();
      const wv = u16(setup + 2), wi = u16(setup + 4), wl = u16(setup + 6), dataOff = rec + 64, ln = wl >= 4 ? 4 : wl;
      out.push(1, 0x40, 0x05, wv & 0xff, (wv >> 8) & 0xff, wi & 0xff, (wi >> 8) & 0xff, ln & 0xff, (ln >> 8) & 0xff);
      for (let i = 0; i < ln; i++) out.push(dataOff + i < rec + incl ? buf[dataOff + i] : 0);
      nw++;
    } else if (xfer === 2 && buf[setup + 1] === 0x05 && buf[setup] === 0xc0) {
      const addr = (u16(setup + 2) | (u16(setup + 4) << 16)) >>> 0;
      if (addr !== runAddr) flush();
      runAddr = addr; runCount++; runVal = ret.get(idKey(h)) ?? runVal;
    } else if (xfer === 3 && epnum === 0x07) {
      flush();
      const dataOff = rec + 64, ln = Math.min(u32(h + 36), incl - 64);
      if (ln > 0) { out.push(2, 7, ln & 0xff, (ln >> 8) & 0xff); for (let i = 0; i < ln; i++) out.push(buf[dataOff + i]); nb++; }
    }
  }
  p = rec + incl;
}
flush();
const blob = Uint8Array.from(out);
Deno.writeFileSync(outPath, blob);

// locate the last fwdl header (kind2 ep7 len112) and its post-fwdl tail (the self-contained monitor bring-up)
const hdrs: number[] = [];
let q = 0;
while (q < blob.length) {
  const k = blob[q];
  if (k === 1) q += 9 + (blob[q + 7] | (blob[q + 8] << 8));
  else if (k === 3) q += 5; else if (k === 4) q += 9;
  else if (k === 2) { const ep = blob[q + 1], ln = blob[q + 2] | (blob[q + 3] << 8); if (ep === 7 && ln === 112) hdrs.push(q); q += 4 + ln; }
  else break;
}
let tail = -1;
if (hdrs.length) {
  let r = hdrs[hdrs.length - 1], sawHdr = 0, inSec = 0;
  while (r < blob.length) {
    const k = blob[r];
    if (k === 2) { const ep = blob[r + 1], ln = blob[r + 2] | (blob[r + 3] << 8); if (ep === 7 && ln === 112) sawHdr = 1; else if (ep === 7 && sawHdr) inSec = 1; r += 4 + ln; }
    else if (k === 1) { if (inSec) { tail = r; break; } r += 9 + (blob[r + 7] | (blob[r + 8] << 8)); }
    else if (k === 3) { if (inSec) { tail = r; break; } r += 5; }
    else if (k === 4) { if (inSec) { tail = r; break; } r += 9; }
    else break;
  }
}
console.log(`devnum=${devnum}  writes=${nw} reads=${nr} polls=${npoll} bulks=${nb}  -> ${outPath} (${blob.length} bytes)`);
console.log(`fwdl cycles=${hdrs.length}; last-cycle post-fwdl tail = byte ${tail}`);
console.log(`replay: sudo ./hwdriver ${outPath} ${tail} hb`);
