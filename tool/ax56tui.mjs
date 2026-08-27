#!/usr/bin/env -S deno run --allow-read --allow-run --allow-env
// ax56tui — a fast live TUI over the AX56 no-root scanner. Streams `C <ch>` / `R <hex>` from ax56ch (via
// termux-usb), parses beacons + client frames, and paints a channel MAP: each channel → its APs → their
// clients, plus the unassociated (probing) stations. Keys: q quit · 5 toggle 5GHz · p pause hop · [/] dwell.
//   deno run -A ax56tui.mjs [device]         # live (spawns termux-usb)
//   deno run -A ax56tui.mjs --replay <file>  # replay a captured C/R stream (dev)
import { parseUnits, BCAST, NIL, isRandomMac } from "./scanparse.mjs";

const TOOL = "/root/ax56-ctl/tool";
const args = Deno.args;
const replay = args.includes("--replay") ? args[args.indexOf("--replay") + 1] : null;
let device = args.find((a) => a.startsWith("/dev/")) || null;

// ── model ────────────────────────────────────────────────────────────────────────────────────────────────
const aps = new Map();       // bssid  -> { ssid, ch, rssi, count, last, clients:Map(mac->{rssi,count,last,assoc}) }
const unassoc = new Map();   // mac    -> { rssi, count, last, probes:Set }
let curCh = 0, totalFr = 0, band5 = false, paused = false, dwell = 300;
const t0 = Date.now();
const now = () => Date.now();
const ema = (o, r) => (o == null ? r : Math.round(o * 0.7 + r * 0.3));

function seenAP(a3, u) {
  const d = aps.get(a3) || { ssid: null, ch: null, rssi: null, count: 0, last: 0, clients: new Map() };
  d.count++; d.last = now(); if (u.rssi != null) d.rssi = ema(d.rssi, u.rssi);
  if (u.ssid != null && u.ssid !== "") d.ssid = u.ssid; if (u.channel != null) d.ch = u.channel; else if (!d.ch) d.ch = curCh;
  aps.set(a3, d); return d;
}
function seenClient(bssid, mac, u, assoc) {
  if (mac === BCAST || mac === NIL) return;
  const ap = aps.get(bssid); if (!ap) return;                 // only attach to an AP we actually know
  const c = ap.clients.get(mac) || { rssi: null, count: 0, last: 0, assoc: false };
  c.count++; c.last = now(); c.assoc = c.assoc || assoc; if (u.rssi != null) c.rssi = ema(c.rssi, u.rssi);
  ap.clients.set(mac, c); unassoc.delete(mac);                // promoted to a known AP's client
}
function seenProbe(mac, u) {
  if (mac === BCAST || mac === NIL) return;
  for (const ap of aps.values()) if (ap.clients.has(mac)) return;   // already an associated client
  const c = unassoc.get(mac) || { rssi: null, count: 0, last: 0, probes: new Set() };
  c.count++; c.last = now(); if (u.rssi != null) c.rssi = ema(c.rssi, u.rssi); if (u.ssid) c.probes.add(u.ssid);
  unassoc.set(mac, c);
}
function ingest(u) {
  totalFr++;
  if (u.type === 0 && (u.subtype === 8 || u.subtype === 5)) seenAP(u.a3, u);        // beacon / probe-resp
  else if (u.type === 0 && u.subtype === 4) seenProbe(u.a2, u);                     // probe-request (station)
  else if (u.toDS && !u.fromDS) seenClient(u.a1, u.a2, u, true);                    // STA -> AP
  else if (u.fromDS && !u.toDS) seenClient(u.a2, u.a1, u, true);                    // AP -> STA
}
function feedLine(line) {
  if (line[0] === "C") { curCh = parseInt(line.slice(2)) || curCh; return; }
  if (line[0] === "R") { const hx = line.slice(2).trim(); if (hx.length >= 32) { const b = new Uint8Array(hx.length / 2); for (let i = 0; i < b.length; i++) b[i] = parseInt(hx.slice(i * 2, i * 2 + 2), 16); for (const u of parseUnits(b)) ingest(u); } }
}

// ── render ───────────────────────────────────────────────────────────────────────────────────────────────
const E = "\x1b[";
const c = { reset: E + "0m", dim: E + "2m", bold: E + "1m", accent: E + "38;5;207m", ok: E + "38;5;77m", warn: E + "38;5;179m", bad: E + "38;5;167m", ink: E + "38;5;250m", mute: E + "38;5;242m" };
const sigColor = (r) => r == null ? c.mute : r >= -55 ? c.ok : r >= -72 ? c.warn : c.bad;
const BARS = "▁▂▃▄▅▆▇█";
const bar = (r) => { if (r == null) return "   "; const n = Math.max(0, Math.min(7, Math.round((r + 92) / 8))); return BARS[Math.min(7, n + 2)].repeat(1) + BARS[Math.min(7, n + 1)] + BARS[n]; };
const age = (t) => { const s = (now() - t) / 1000; return s < 8 ? c.ok : s < 25 ? c.warn : c.mute; };
const pad = (s, n) => (s + " ".repeat(n)).slice(0, n);
const mmss = (ms) => { const s = Math.floor(ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; };

let status = "starting";
function render() {
  let sz; try { sz = Deno.consoleSize(); } catch { sz = { columns: 100, rows: 40 }; }
  const { columns: W, rows: H } = sz;
  const out = [];
  const push = (s) => { if (out.length < H - 1) out.push(s); };
  const nAP = aps.size, nSTA = [...aps.values()].reduce((a, d) => a + d.clients.size, 0) + unassoc.size;
  const hop = paused ? `${c.warn}paused${c.reset}` : `${c.accent}↻ ${band5 ? "2.4+5" : "2.4"}GHz${c.reset}`;
  push(`${c.bold}${c.accent} AX56 ${c.reset}${c.dim}scanner${c.reset}   ch ${c.bold}${String(curCh).padStart(3)}${c.reset} ${hop}   ${c.dim}up${c.reset} ${mmss(now() - t0)}   ${c.ink}${totalFr}${c.reset}${c.dim}fr${c.reset}   ${c.accent}${nAP}${c.reset} AP · ${c.accent}${nSTA}${c.reset} STA   ${c.dim}${status}${c.reset}`);
  // channel occupancy strip (deep per-channel: AP+client counts)
  const perCh = new Map();
  for (const d of aps.values()) { const k = d.ch || 0; const e = perCh.get(k) || { ap: 0, cl: 0 }; e.ap++; e.cl += d.clients.size; perCh.set(k, e); }
  const chans = band5 ? [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 36, 40, 44, 48, 149, 153, 157, 161, 165] : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  const strip = chans.map((ch) => { const e = perCh.get(ch); const on = ch === curCh; const col = on ? c.accent : e ? c.ink : c.mute; return `${col}${e ? BARS[Math.min(7, e.ap + e.cl)] : "·"}${c.reset}`; }).join("");
  push(` ${c.dim}band${c.reset} ${strip}  ${c.dim}(bar = APs+clients per channel)${c.reset}`);
  push(`${c.dim} ${"─".repeat(Math.min(W - 2, 76))}${c.reset}`);
  push(` ${c.dim}CH  PWR  ESSID              BSSID / STATION      DATA  SEEN${c.reset}`);
  // the MAP: APs grouped by channel, sorted by channel then signal, each AP with its clients nested
  const apList = [...aps.entries()].map(([b, d]) => ({ b, ...d })).sort((x, y) => (x.ch - y.ch) || ((y.rssi ?? -999) - (x.rssi ?? -999)));
  for (const a of apList) {
    if (out.length >= H - 3) { push(`   ${c.dim}… more (narrow to APs by signal)${c.reset}`); break; }
    const s = sigColor(a.rssi);
    push(` ${c.bold}${String(a.ch ?? "·").padStart(2)}${c.reset}  ${s}${bar(a.rssi)}${String(a.rssi ?? "—").padStart(4)}${c.reset} ${c.ink}${pad(a.ssid || "‹hidden›", 18)}${c.reset} ${c.mute}${a.b}${c.reset} ${String(a.count).padStart(5)} ${age(a.last)}●${c.reset}`);
    const cls = [...a.clients.entries()].map(([m, d]) => ({ m, ...d })).sort((x, y) => (y.rssi ?? -999) - (x.rssi ?? -999));
    for (const cl of cls) {
      if (out.length >= H - 3) break;
      const cs = sigColor(cl.rssi);
      push(`        ${cs}${String(cl.rssi ?? "—").padStart(4)}${c.reset} ${c.dim}└─${c.reset} ${c.ink}${cl.m}${c.reset}${isRandomMac(cl.m) ? c.dim + " rnd" + c.reset : ""} ${String(cl.count).padStart(4)} ${age(cl.last)}●${c.reset}`);
    }
  }
  // unassociated (probing) stations
  const un = [...unassoc.entries()].map(([m, d]) => ({ m, ...d })).sort((x, y) => (y.rssi ?? -999) - (x.rssi ?? -999));
  if (un.length && out.length < H - 3) {
    push(`${c.dim} ── ${un.length} unassociated (probing) ${"─".repeat(Math.max(0, Math.min(W - 30, 44)))}${c.reset}`);
    let shown = 0;
    for (const u of un) {
      if (out.length >= H - 2) { push(`   ${c.dim}… +${un.length - shown} more${c.reset}`); break; }
      shown++;
      const cs = sigColor(u.rssi), pr = [...u.probes].filter(Boolean).slice(0, 3).join(" ");
      push(`     ${cs}${String(u.rssi ?? "—").padStart(4)}${c.reset} ${c.ink}${u.m}${c.reset}${isRandomMac(u.m) ? c.dim + " rnd" + c.reset : ""} ${String(u.count).padStart(4)}${pr ? c.dim + "  → " + pr + c.reset : ""}`);
    }
  }
  const footer = `${c.dim} q quit · 5 band · p pause · [ ] dwell ${dwell}ms${c.reset}`;
  let buf = E + "H" + E + "0J";                                // home + clear-below
  buf += out.join(E + "0K\r\n") + E + "0K\r\n";
  buf += E + `${H};1H` + footer + E + "0K";
  Deno.stdout.writeSync(new TextEncoder().encode(buf));
}

// ── sources ──────────────────────────────────────────────────────────────────────────────────────────────
async function pump(readable) {
  let buf = "";
  for await (const chunk of readable.pipeThrough(new TextDecoderStream())) {
    buf += chunk; let i;
    while ((i = buf.indexOf("\n")) >= 0) { const line = buf.slice(0, i); buf = buf.slice(i + 1); if (line) feedLine(line); }
  }
}
let child = null;
async function startLive() {
  if (!device) { status = "no device — pass /dev/bus/usb/BBB/DDD"; return; }
  status = "bringing up (cold ~20s)…";
  const env = { ...Deno.env.toObject(), DWELL: String(dwell), LOOP: "1" }; if (band5) env.SCAN5 = "1"; else delete env.SCAN5;
  const cmd = new Deno.Command("termux-usb", { args: ["-r", "-e", `${TOOL}/stream_cb.sh`, device], stdout: "piped", stderr: "null", env });
  child = cmd.spawn();
  pump(child.stdout).catch(() => {}).finally(() => { status = "stream ended — press 5/p to restart or q"; });
  child.status.then(() => { status = "adapter released"; });
  setTimeout(() => { if (totalFr > 0) status = "live"; }, 1500);
}
function stopLive() { try { child?.kill("SIGTERM"); } catch { /* */ } child = null; }
async function startReplay(file) {
  status = "replay " + file;
  const lines = (await Deno.readTextFile(file)).split("\n");
  let i = 0; const step = () => { for (let k = 0; k < 60 && i < lines.length; k++, i++) feedLine(lines[i]); if (i >= lines.length) { i = 0; curCh = 0; } setTimeout(step, 40); }; step();
}

// ── input + loop ─────────────────────────────────────────────────────────────────────────────────────────
function cleanup() { try { Deno.stdin.setRaw(false); } catch { /* */ } Deno.stdout.writeSync(new TextEncoder().encode(E + "?25h" + E + "2J" + E + "H")); }
async function keys() {
  try { Deno.stdin.setRaw(true); } catch { /* not a tty (piped) */ return; }
  const b = new Uint8Array(8);
  while (true) {
    const n = await Deno.stdin.read(b); if (n === null) break;
    const k = String.fromCharCode(b[0]);
    if (k === "q" || b[0] === 3) { cleanup(); stopLive(); Deno.exit(0); }
    else if (k === "5" && !replay) { band5 = !band5; stopLive(); await startLive(); }
    else if (k === "p" && !replay) { paused = !paused; if (paused) stopLive(); else await startLive(); }
    else if (k === "[") { dwell = Math.max(120, dwell - 60); }
    else if (k === "]") { dwell = Math.min(1500, dwell + 60); }
  }
}

Deno.stdout.writeSync(new TextEncoder().encode(E + "?25l" + E + "2J"));   // hide cursor, clear
if (replay) startReplay(replay); else startLive();
keys();
setInterval(render, 350);
globalThis.addEventListener("unload", cleanup);
