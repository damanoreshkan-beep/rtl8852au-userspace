// ax56 chat carrier (TX side) — turn a message into hwdriver INJECT packets: an AX56CHAT beacon per meshchat
// fragment, each prefixed with the proven 48-byte txdesc (pktsize patched to the frame length) and a 2-byte
// length header, exactly the [len:2LE][txdesc][frame] shape hwdriver's INJECT loop consumes (verified against
// tx_mark.bin: 3× [5a 00][48B txdesc][42B probe-req]; txdesc word2 low16 = frame length).
//
//   deno run -A carrier.ts <room> <message> [--pass <p>] [--reps N] [--src HEX] [--out FILE]
//
// Uses the real farm protocol (meshchat) + crypto (meshcrypto), so the wire matches what a receiver decodes.
import { roomId, fragment, encodeChunk, textBytes, FLAG_ENCRYPTED } from "file:///root/microspec/packages/runtime/meshchat.js";
import { deriveKey, seal } from "file:///root/microspec/packages/runtime/meshcrypto.js";

const a = Deno.args;
const room = a[0] || "air";
const msg = a[1] || "hello from ax56";
const opt = (n, d = null) => { const i = a.indexOf(n); return i >= 0 ? a[i + 1] : d; };
const pass = opt("--pass");
const reps = Number(opt("--reps", "1"));
const src = (opt("--src") ? parseInt(opt("--src"), 16) : 0xa11ce511) >>> 0;
const out = opt("--out", "/root/ax56-ctl/tool/carrier_tx.bin");

const OUI = [0x00, 0x16, 0x3e, 0x01];
const bssid = [0x02, (src >>> 24) & 0xff, (src >>> 16) & 0xff, (src >>> 8) & 0xff, src & 0xff, 0x01];

// the proven txdesc is tx_mark's packet minus its 2-byte len prefix (bytes 2..49)
const txmark = await Deno.readFile("/root/ax56-ctl/tool/tx_mark.bin");
const TXD = txmark.subarray(2, 2 + 48);

function beaconFrame(chunk: Uint8Array): number[] {
  const ssid = [...textBytes("AX56CHAT")];
  return [
    0x80, 0x00, 0x00, 0x00,                                   // fc=beacon, duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                       // addr1 broadcast
    ...bssid, ...bssid,                                        // addr2 SA, addr3 BSSID
    0x00, 0x00,                                                // seq
    0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x00, 0x00,          // timestamp, interval, capability
    0x00, ssid.length, ...ssid,                               // SSID IE
    0xdd, OUI.length + chunk.length, ...OUI, ...chunk,        // vendor IE + meshchat chunk
    0x03, 0x01, 0x06,                                          // DS param (channel 6)
  ];
}

function packet(chunk: Uint8Array): number[] {
  const frame = beaconFrame(chunk);
  const L = frame.length;
  const txd = Uint8Array.from(TXD);
  txd[8] = L & 0xff; txd[9] = (L >>> 8) & 0xff;               // txdesc word2 low16 = pktsize (frame length)
  const pkt = [...txd, ...frame];
  return [pkt.length & 0xff, (pkt.length >>> 8) & 0xff, ...pkt]; // 2-byte len prefix for hwdriver INJECT
}

const rid = roomId(room);
let blob = textBytes(msg), flags = 0;
if (pass) { blob = await seal(await deriveKey(pass, room), textBytes(msg)); flags = FLAG_ENCRYPTED; }
const parts = fragment(blob);
const total = parts.length;

const bytes: number[] = [];
for (let r = 0; r < reps; r++) {
  for (let i = 0; i < total; i++) {
    const chunk = encodeChunk({ room: rid, src, msgId: 1, frag: i, total, flags, payload: parts[i] });
    bytes.push(...packet(chunk));
  }
}
await Deno.writeFile(out, Uint8Array.from(bytes));
console.log(`${out}: ${bytes.length}B — ${total} fragment(s) × ${reps} rep(s), enc=${flags & 1}, room=0x${rid.toString(16)}, src=0x${src.toString(16)}`);
console.log(`frame len per fragment: ${parts.map((p) => 36 + 10 + 2 + OUI.length + 14 + p.length).join(", ")} B`);
