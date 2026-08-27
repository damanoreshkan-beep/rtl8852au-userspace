// scan_table.mjs — turn ax56ch SCAN output (`R <hex>` EP0x84 transfers on stdin) into an airodump-style table.
// Self-contained: the RTL8852AU rxd + 802.11 beacon/PPDU parse (RSSI = (raw>>1)-110), aggregated by BSSID.
const dec = new TextDecoder();
const mac = (b, o) => Array.from(b.subarray(o, o + 6), (x) => x.toString(16).padStart(2, "0")).join(":");
const fromHex = (h) => { const u = new Uint8Array(h.length / 2); for (let i = 0; i < u.length; i++) u[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16); return u; };

let curSig = null;   // last PPDU-status RSSI, carried across transfers (a beacon's status can land in a prior one)
function parseUnits(rx) {
  const out = []; const u32 = (o) => (rx[o] | (rx[o + 1] << 8) | (rx[o + 2] << 16) | (rx[o + 3] << 24)) >>> 0;
  let off = 0, guard = 0;
  while (off + 16 <= rx.length && guard++ < 256) {
    const d0 = u32(off);
    const pktsize = d0 & 0x3fff, shift = (d0 >> 14) & 3, rt = (d0 >> 24) & 0xf, drvsize = (d0 >> 28) & 7, rxdlen = ((d0 >>> 31) & 1) ? 32 : 16;
    if (pktsize === 0) break;
    const foff = off + rxdlen + drvsize * 8 + shift;
    if (rt === 1) {
      const po = foff;
      if (po + 8 <= rx.length) {
        const iw0 = u32(po), iw1 = u32(po + 4), usr = iw0 & 0xf, rxcnt = (iw0 >>> 29) & 1, plcp = ((iw1 >>> 16) & 0xff) << 3;
        const hs = po + 8 + usr * 4 + ((usr & 1) ? 4 : 0) + (rxcnt ? 96 : 0) + plcp;
        if (hs + 8 <= rx.length) { const hw0 = u32(hs), hw1 = u32(hs + 4), valid = (hw0 >>> 7) & 1, raw = Math.max(hw1 & 0xff, (hw1 >>> 8) & 0xff); if (valid && raw) curSig = (raw >> 1) - 110; }
      }
    } else if (rt === 0 && pktsize >= 24 && foff + pktsize <= rx.length) {
      const fc = rx[foff] | (rx[foff + 1] << 8);
      const u = { rssi: curSig, type: (fc >> 2) & 3, subtype: (fc >> 4) & 0xf, toDS: (fc >> 8) & 1, fromDS: (fc >> 9) & 1,
        a1: mac(rx, foff + 4), a2: mac(rx, foff + 10), a3: mac(rx, foff + 16), ssid: null, channel: null };
      if (u.type === 0 && (u.subtype === 8 || u.subtype === 5)) {
        let p = foff + 24 + 12; const end = foff + pktsize;
        while (p + 2 <= end) { const tag = rx[p], len = rx[p + 1]; if (p + 2 + len > end) break;
          if (tag === 0 && len <= 32) { try { u.ssid = dec.decode(rx.subarray(p + 2, p + 2 + len)); } catch { u.ssid = ""; } }
          else if (tag === 3 && len >= 1) u.channel = rx[p + 2]; p += 2 + len; }
      }
      out.push(u);
    }
    let unit = rxdlen + drvsize * 8 + shift + pktsize; unit = (unit + 7) & ~7; off += unit;
  }
  return out;
}

const BCAST = "ff:ff:ff:ff:ff:ff", NIL = "00:00:00:00:00:00";
const aps = new Map();        // bssid  -> { ssid, channel, rssi, count }
const stas = new Map();       // client -> { ap, rssi, count }
const text = await new Response(Deno.stdin.readable).text();
let frames = 0;
for (const line of text.split("\n")) {
  if (!line.startsWith("R ")) continue;
  frames++;
  for (const u of parseUnits(fromHex(line.slice(2).trim()))) {
    if (u.type === 0 && (u.subtype === 8 || u.subtype === 5)) {          // beacon / probe-resp => an AP
      const d = aps.get(u.a3) || { ssid: null, channel: null, rssi: null, count: 0 };
      d.count++; if (u.ssid != null && u.ssid !== "") d.ssid = u.ssid; if (u.channel != null) d.channel = u.channel;
      if (u.rssi != null && (d.rssi == null || u.rssi > d.rssi)) d.rssi = u.rssi;
      aps.set(u.a3, d);
    } else {
      // a STA heard TRANSMITTING: to-DS data/mgmt (a2=client, a1=AP), or a probe-request (a2=client, unassoc).
      const toDS = u.toDS && !u.fromDS, probeReq = u.type === 0 && u.subtype === 4;
      if ((toDS && (u.type === 2 || u.type === 0)) || probeReq) {
        if (u.a2 !== BCAST && u.a2 !== NIL) {
          const ap = toDS && u.a1 !== BCAST && u.a1 !== NIL ? u.a1 : null;
          const d = stas.get(u.a2) || { ap: null, rssi: null, count: 0 };
          d.count++; if (ap) d.ap = ap;
          if (u.rssi != null && (d.rssi == null || u.rssi > d.rssi)) d.rssi = u.rssi;
          stas.set(u.a2, d);
        }
      }
    }
  }
}
for (const b of aps.keys()) stas.delete(b);   // an AP is not a client

const pad = (s, n) => String(s).padEnd(n), rpad = (s, n) => String(s).padStart(n), dim = (s) => `\x1b[2m${s}\x1b[0m`;
const bySig = (a, b) => (b.rssi ?? -999) - (a.rssi ?? -999);
const apRows = [...aps.entries()].map(([bssid, d]) => ({ bssid, ...d })).sort(bySig);
const staRows = [...stas.entries()].map(([mac, d]) => ({ mac, ...d })).sort(bySig);
const ssidOf = (b) => (aps.get(b) && aps.get(b).ssid) || null;

console.log(`\n  PWR  CH   #    BSSID              ESSID`);
console.log(`  ${"─".repeat(54)}`);
for (const r of apRows) console.log(`  ${rpad(r.rssi ?? "—", 3)}  ${rpad(r.channel ?? "·", 2)}  ${rpad(r.count, 4)}  ${pad(r.bssid, 17)}  ${r.ssid || dim("<hidden>")}`);

console.log(`\n  PWR   #    STATION            BSSID              PROBING`);
console.log(`  ${"─".repeat(64)}`);
for (const r of staRows) {
  const ap = r.ap ? `${r.ap}${ssidOf(r.ap) ? " " + ssidOf(r.ap) : ""}` : dim("(not associated)");
  console.log(`  ${rpad(r.rssi ?? "—", 3)}  ${rpad(r.count, 4)}  ${pad(r.mac, 17)}  ${pad(r.ap || "", 17)}  ${r.ap ? (ssidOf(r.ap) || dim("<hidden>")) : dim("—")}`);
}
console.log(`\n  ${apRows.length} AP(s) · ${staRows.length} station(s) · ${frames} RX transfers\n`);
