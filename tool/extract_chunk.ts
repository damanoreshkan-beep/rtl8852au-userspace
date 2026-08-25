// ax56 chat carrier (RX side) — scan a radiotap pcap (written by the hwdriver EP0x84 read) for our vendor IE
// (OUI 00:16:3e + sub-type 01), decode meshchat chunks with the REAL farm codec, DEDUPE the carrier's repeats,
// REASSEMBLE fragments into whole messages, and (with --pass) decrypt the AES-GCM box. The on-air proof that
// a chat message survived the wifi carry end-to-end, no-root.
//
//   deno run -A extract_chunk.ts [pcap] [--pass <passphrase>] [--room <name>]
import { decodeChunk, bytesText, Deduper, Reassembler, FLAG_ENCRYPTED } from "file:///root/microspec/packages/runtime/meshchat.js";
import { deriveKey, open } from "file:///root/microspec/packages/runtime/meshcrypto.js";

const a = Deno.args;
const pcap = a.find((x) => !x.startsWith("--") && a.indexOf(x) === 0) || "/tmp/ax56.pcap";
const opt = (n) => { const i = a.indexOf(n); return i >= 0 ? a[i + 1] : null; };
const pass = opt("--pass");
const room = opt("--room") || "air";
const key = pass ? await deriveKey(pass, room) : null;

const buf = await Deno.readFile(pcap);
const sig = [0x00, 0x16, 0x3e, 0x01];
const dedup = new Deduper(), rasm = new Reassembler();
let hits = 0;
const messages: { src: number; text: string }[] = [];

for (let i = 1; i + 4 < buf.length; i++) {
  if (buf[i] === sig[0] && buf[i + 1] === sig[1] && buf[i + 2] === sig[2] && buf[i + 3] === sig[3]) {
    const clen = buf[i - 1] - 4;                 // vendor IE length byte − OUI(3) − sub-type(1)
    if (clen < 14 || i + 4 + clen > buf.length) continue;
    const d = decodeChunk(buf.subarray(i + 4, i + 4 + clen));
    if (!d) continue;
    hits++;
    if (dedup.seen(d.src, d.msgId, d.frag)) continue;
    const done = rasm.add(d);
    if (!done) continue;
    let blob = done.blob;
    if (done.flags & FLAG_ENCRYPTED) {
      if (!key) { console.log(`(encrypted message from 0x${done.src.toString(16)} — pass --pass to read)`); continue; }
      const pt = await open(key, done.blob);
      if (!pt) { console.log(`(message from 0x${done.src.toString(16)} did not decrypt — wrong key)`); continue; }
      blob = pt;
    }
    messages.push({ src: done.src, text: bytesText(blob) });
  }
}

for (const m of messages) console.log(`[${(m.src >>> 0).toString(16).padStart(8, "0")}] ${m.text}`);
console.log(`\nvendor-IE hits: ${hits}, messages reassembled: ${messages.length}`);
console.log(messages.length ? ">>> chat message(s) crossed the air and decoded no-root <<<" : ">>> nothing reassembled <<<");
