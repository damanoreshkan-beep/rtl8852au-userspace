// Build a 228-byte 802.11 beacon carrying a real meshchat chunk in a vendor-specific IE, prefixed with the
// proven 48-byte txdesc from tx_mark.bin (frame length unchanged = 228, so the descriptor stays valid).
// Uses the ACTUAL farm protocol encoder so the on-air test validates the shipping codec.
import { encodeChunk, textBytes, roomId } from "file:///root/microspec/packages/runtime/meshchat.js";

const OUI = [0x00, 0x16, 0x3e], VTYPE = 0x01;          // our vendor IE (OUI + sub-type)
const msg = Deno.args[0] || "HELLO-AX56CHAT";
const chunk = encodeChunk({ room: roomId("air"), src: 0xa1b2c3d4, msgId: 1, frag: 0, total: 1, flags: 0, payload: textBytes(msg) });

const F = new Uint8Array(228);
let p = 0;
const put = (...b: number[]) => { for (const x of b) F[p++] = x & 0xff; };
put(0x80, 0x00, 0x00, 0x00);                          // fc=beacon, duration
put(0xff, 0xff, 0xff, 0xff, 0xff, 0xff);              // addr1 broadcast
put(0x02, 0x11, 0x22, 0x33, 0x44, 0x55);             // addr2 SA
put(0x02, 0x11, 0x22, 0x33, 0x44, 0x55);             // addr3 BSSID
put(0x00, 0x00);                                      // seq
for (let i = 0; i < 8; i++) put(0x00);                // timestamp
put(0x64, 0x00);                                      // beacon interval 100
put(0x00, 0x00);                                      // capability
const ssid = textBytes("AX56CHAT");                   // SSID IE — easy to spot in tcpdump
put(0x00, ssid.length, ...ssid);
put(0xdd, OUI.length + 1 + chunk.length, ...OUI, VTYPE, ...chunk); // vendor IE with the chunk
const remain = 228 - p;                               // pad to exactly 228 with a filler vendor IE (zeros)
put(0xdd, remain - 2);
p = 228;

const txmark = await Deno.readFile("/root/ax56-ctl/tool/tx_mark.bin");
const out = new Uint8Array(48 + 228);
out.set(txmark.subarray(0, 48), 0);
out.set(F, 48);
await Deno.writeFile("/root/ax56-ctl/tool/chatframe.bin", out);

const hex = (u: Uint8Array) => Array.from(u, (x) => x.toString(16).padStart(2, "0")).join("");
console.log(`chatframe.bin: ${out.length}B (48 txdesc + 228 beacon); msg="${msg}"`);
console.log(`chunk (${chunk.length}B) in vendor IE, hex: ${hex(chunk)}`);
console.log(`payload hex to grep in the capture: ${hex(textBytes(msg))}`);
