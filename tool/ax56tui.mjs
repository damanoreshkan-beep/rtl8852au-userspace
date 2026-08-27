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
let lockedCh = 0, sortMode = 0, prevFr = 0, prevT = Date.now(), pps = 0, spinI = 0;
const SORTS = [["ch", "channel"], ["sig", "signal"], ["cli", "clients"], ["data", "packets"]];
const SPIN = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
const CTL = `${TOOL}/ax56tui.ctl`;
const writeCtl = () => { try { Deno.writeTextFileSync(CTL, `${lockedCh} ${dwell}`); } catch { /* */ } };
const setLock = (ch) => { lockedCh = ch; writeCtl(); };
const HOPCH = () => (band5 ? [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 36, 40, 44, 48, 149, 153, 157, 161, 165] : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]);
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
const rpad4 = (v) => String(v ?? "—").padStart(4);
function render() {
  let sz; try { sz = Deno.consoleSize(); } catch { sz = { columns: 72, rows: 40 }; }
  const W = parseInt(Deno.env.get("COLS") || "") || sz.columns, H = parseInt(Deno.env.get("ROWS") || "") || sz.rows;
  if (totalFr > 0 && (status.startsWith("tap") || status.startsWith("bringing"))) status = "live";
  const out = [];
  const push = (s) => { if (out.length < H - 1) out.push(s); };
  const nAP = aps.size, nSTA = [...aps.values()].reduce((a, d) => a + d.clients.size, 0) + unassoc.size;
  // responsive columns for a phone terminal: BSSID + packets/age drop first as the screen narrows.
  const showB = W >= 56, showData = W >= 46, indent = W < 40 ? 2 : 4;
  const essidW = Math.max(7, Math.min(20, W - (showB ? 44 : showData ? 24 : 18)));
  const clip = (s, n) => (s.length > n ? s.slice(0, n - 1) + "…" : s);

  const nowt = now(), dt = (nowt - prevT) / 1000;              // packets-per-second + a pulse that spins while frames flow
  if (dt >= 0.25) { pps = Math.round((totalFr - prevFr) / dt); prevFr = totalFr; prevT = nowt; }
  if (pps > 0) spinI = (spinI + 1) % SPIN.length;
  const rx = pps > 0 ? `${c.ok}${SPIN[spinI]}${c.reset}` : `${c.mute}·${c.reset}`;
  const hop = lockedCh ? `${c.warn}▣${lockedCh}${c.reset}` : paused ? `${c.warn}paused${c.reset}` : `${c.accent}↻${band5 ? "2.4+5" : "2.4"}${c.reset}`;
  push(`${c.bold}${c.accent}AX56${c.reset} ${c.dim}ch${c.reset}${c.bold}${String(curCh).padStart(3)}${c.reset} ${hop} ${rx}${c.ink}${String(pps).padStart(3)}${c.reset}${c.dim}/s${c.reset} ${c.accent}${nAP}${c.reset}${c.dim}ap${c.reset} ${c.accent}${nSTA}${c.reset}${c.dim}sta ${clip(status, Math.max(4, W - 42))}${c.reset}`);
  // channel occupancy strip — deep per-channel view: one bar per channel, height = APs+clients there
  const perCh = new Map();
  for (const d of aps.values()) { const k = d.ch || 0; const e = perCh.get(k) || 0; perCh.set(k, e + 1 + d.clients.size); }
  const chans = band5 ? [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 36, 40, 44, 48, 149, 153, 157, 161, 165] : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  const strip = chans.map((ch) => { const e = perCh.get(ch); const col = ch === curCh ? c.accent : e ? c.ink : c.mute; return `${col}${e ? BARS[Math.min(7, e)] : "·"}${c.reset}`; }).join("");
  push(`${c.dim}ch${c.reset} ${strip}`);
  push(`${c.dim}${"─".repeat(Math.min(W, 80))}${c.reset}`);
  // the MAP: APs sorted by the chosen key, each AP with its clients nested
  const sorters = { ch: (x, y) => (x.ch - y.ch) || ((y.rssi ?? -999) - (x.rssi ?? -999)), sig: (x, y) => (y.rssi ?? -999) - (x.rssi ?? -999), cli: (x, y) => (y.clients.size - x.clients.size) || ((y.rssi ?? -999) - (x.rssi ?? -999)), data: (x, y) => y.count - x.count };
  const apList = [...aps.entries()].map(([b, d]) => ({ b, ...d })).sort(sorters[SORTS[sortMode][0]]);
  for (const a of apList) {
    if (out.length >= H - 3) { push(`${c.dim} … more${c.reset}`); break; }
    const s = sigColor(a.rssi);
    const tail = showData ? ` ${c.mute}${String(a.count)}${c.reset} ${age(a.last)}●${c.reset}` : "";
    push(`${c.bold}${String(a.ch ?? "·").padStart(2)}${c.reset} ${s}${bar(a.rssi)}${rpad4(a.rssi)}${c.reset} ${c.ink}${pad(clip(a.ssid || "‹hidden›", essidW), essidW)}${c.reset}${showB ? " " + c.mute + a.b + c.reset : ""}${tail}`);
    const cls = [...a.clients.entries()].map(([m, d]) => ({ m, ...d })).sort((x, y) => (y.rssi ?? -999) - (x.rssi ?? -999));
    for (const cl of cls) {
      if (out.length >= H - 3) break;
      push(`${" ".repeat(indent)}${sigColor(cl.rssi)}${rpad4(cl.rssi)}${c.reset} ${c.dim}└${c.reset}${c.ink}${cl.m}${c.reset}${isRandomMac(cl.m) ? c.dim + "~" + c.reset : ""}${showData ? " " + c.mute + cl.count + c.reset : ""}`);
    }
  }
  // unassociated (probing) stations
  const un = [...unassoc.entries()].map(([m, d]) => ({ m, ...d })).sort((x, y) => (y.rssi ?? -999) - (x.rssi ?? -999));
  if (un.length && out.length < H - 3) {
    push(`${c.dim}── ${un.length} probing ${"─".repeat(Math.max(0, Math.min(W - 12, 60)))}${c.reset}`);
    let shown = 0;
    for (const u of un) {
      if (out.length >= H - 2) { push(`${c.dim} … +${un.length - shown}${c.reset}`); break; }
      shown++;
      const pr = [...u.probes].filter(Boolean).slice(0, 2).join(" ");
      push(`${" ".repeat(indent)}${sigColor(u.rssi)}${rpad4(u.rssi)}${c.reset} ${c.ink}${u.m}${c.reset}${isRandomMac(u.m) ? c.dim + "~" + c.reset : ""}${pr && W >= 52 ? c.dim + " →" + clip(pr, W - 30) + c.reset : ""}`);
    }
  }
  const footer = clip(`q·quit 5·band p·pause c·lock ,.·ch s·${SORTS[sortMode][1]} [ ]·${dwell}ms`, W);
  let buf = E + "H" + E + "0J" + out.join(E + "0K\r\n") + E + "0K\r\n" + E + `${H};1H` + c.dim + footer + c.reset + E + "0K";
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
const FIFO = `${TOOL}/ax56tui.fifo`;
let child = null, fifoFile = null;
async function startLive() {
  if (!device) { status = "no device — pass /dev/bus/usb/BBB/DDD"; return; }
  status = "tap the USB permission popup… then cold bring-up ~20s";
  try { await new Deno.Command("mkfifo", { args: [FIFO] }).output(); } catch { /* exists */ }
  writeCtl();                                                  // hand the fresh scanner the current lock + dwell
  const env = { ...Deno.env.toObject(), DWELL: String(dwell), LOOP: "1" }; if (band5) env.SCAN5 = "1"; else delete env.SCAN5;
  child = new Deno.Command("termux-usb", { args: ["-r", "-e", `${TOOL}/stream_cb.sh`, device], stdout: "null", stderr: "null", env }).spawn();
  child.status.then(() => { if (!paused) status = "adapter released — 5/p restart · q quit"; });
  (async () => {   // read the FIFO the callback streams into (open rendezvous with the callback's write-open)
    try { fifoFile = await Deno.open(FIFO, { read: true }); await pump(fifoFile.readable); }
    catch (e) { status = "fifo: " + (e.message || e); }
    finally { if (!paused) status = "stream ended — 5/p restart · q quit"; }
  })();
  setTimeout(() => { if (totalFr > 0) status = "live"; }, 1500);
}
function stopLive() { try { child?.kill("SIGTERM"); } catch { /* */ } try { fifoFile?.close(); } catch { /* */ } child = null; fifoFile = null; }
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
    else if (k === "[") { dwell = Math.max(120, dwell - 60); writeCtl(); }
    else if (k === "]") { dwell = Math.min(1500, dwell + 60); writeCtl(); }
    else if (k === "c") { setLock(lockedCh ? 0 : (curCh || HOPCH()[0])); }
    else if (k === "," || k === ".") { const list = HOPCH(); let i = list.indexOf(lockedCh || curCh); if (i < 0) i = 0; i = (i + (k === "." ? 1 : list.length - 1)) % list.length; setLock(list[i]); }
    else if (k === "s") { sortMode = (sortMode + 1) % SORTS.length; }
  }
}

Deno.stdout.writeSync(new TextEncoder().encode(E + "?25l" + E + "2J"));   // hide cursor, clear
if (replay) startReplay(replay); else startLive();
keys();
setInterval(render, 350);
globalThis.addEventListener("unload", cleanup);
