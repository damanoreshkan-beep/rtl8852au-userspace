// ax56tx.ts — no-root userspace RTL8852AU TX chain for the PHONE (Deno + termux-usb fd).
// Faithful port of tool/hwdriver.c + initcal.c + dpk_live.c + tssi_all.c (proven on-air: 899/900).
//
// termux-usb passes ONLY the usbfs fd as argv[0], so every parameter comes from the environment:
//   FD (argv0)  BLOB=<config replay blob>  START=<tail byte offset>  HB=1 (hwburst fwdl first)
//   FW=<fw_cut2_nic.bin>  INITCAL=1 IQKCHAIN=1 TSSILIVE=1 DPKLIVE=1  INJECT=<pkts file> INJECT_REPS=N
//   LOG=<log path, default /tmp/ax56tx.log>
// Run via a termux-usb wrapper, e.g.:
//   termux-usb -r -e '/…/wrap.sh' <dev>   where wrap.sh runs: BLOB=… START=… HB=1 INITCAL=1 … deno run -A ax56tx.ts "$1"
//
// NOTE: fwdl sections and EP7 section-runs are submitted SEQUENTIALLY here (hwdriver.c bursts them async to
// avoid inter-packet gaps). If the MCU NAKs mid-download on the phone, that burst is the thing to revisit.

import {
  claim, bulk, usleep, r32, w32, BB, bbm, bbr, bbset, bbclr, wrf, rrf, RFREG_MASK,
} from "./ax56tx_hw.ts";
import * as T from "./ax56tx_tbls.js";

// ---- logging (file, like hwdriver's /tmp/hwdriver.log) ----
const LOGPATH = Deno.env.get("LOG") || "/tmp/ax56tx.log";
let logFd: Deno.FsFile | null = null;
try { logFd = Deno.openSync(LOGPATH, { create: true, write: true, truncate: true }); } catch { /* */ }
const enc = new TextEncoder();
function P(s: string) { const b = enc.encode(s); try { if (logFd) logFd.writeSync(b); } catch { /* */ } try { Deno.stdout.writeSync(b); } catch { /* */ } }
const env = (k: string) => Deno.env.get(k);
const hex = (n: number) => "0x" + (n >>> 0).toString(16);

// ================= firmware download (hwburst_fwdl) =================
function hwburst_fwdl(fwpath: string): number {
  { const plat = r32(0x88); if (plat & 0x2) { w32(0x88, plat & ~0x2); for (let c = 0; c < 50; c++) r32(0x88); } }
  w32(0xf4, 0x20012248); w32(0x40, 0); w32(0x1c, 0xf38000);
  w32(0x8380, 3);
  w32(0x8400, 0x60440000); w32(0x8404, 0x40000); w32(0x8400, 0x60440000); w32(0x8404, 0x4840000);
  w32(0x8c08, 0); w32(0x9008, 0x402001);
  w32(0x8c40, 0); w32(0x8c44, 0xc4); w32(0x8c4c, 0); w32(0x8c50, 0);
  const q = [0x9040, 0, 0x9044, 0, 0x9048, 0x100010, 0x904c, 0x300030, 0x9050, 0, 0x9054, 0, 0x9058, 0, 0x905c, 0, 0x9060, 0, 0x9064, 0, 0x9068, 0];
  for (let i = 0; i < 22; i += 2) w32(q[i], q[i + 1]);
  w32(0x8400, 0x64c40000);
  w32(0x8a00, 0); w32(0x8a04, 0x200000); w32(0x8a00, 0x400); w32(0x8a00, 0x408);
  w32(0x88, 0x54d);
  { const v = r32(0x1e0) & ~0x7; w32(0x1e0, v); }
  w32(0x8, 0x20ac21);
  w32(0xc04, 0x18003040); w32(0x40000, 0); w32(0xc04, 0x18003044); w32(0xc04, 0x18003044); w32(0x40000, 0x100);
  w32(0x88, 0x54c); w32(0x88, 0x54d);
  w32(0x1f4, 0); w32(0x1f8, 0); w32(0x160, 0); w32(0x164, 0); w32(0x168, 0); w32(0x16c, 0);
  w32(0x8, 0x20ec21); w32(0x1e0, 1); w32(0x88, 0x54f);
  let armed = 0; for (let c = 0; c < 400000; c++) { if (r32(0x1e0) & 2) { armed = 1; break; } }
  if (!armed) { P(`hwburst: H2C not armed 0x1e0=${hex(r32(0x1e0))}\n`); return -1; }
  const fw = Deno.readFileSync(fwpath);
  const LE = (o: number) => (fw[o] | (fw[o + 1] << 8) | (fw[o + 2] << 16) | (fw[o + 3] << 24)) >>> 0;
  const secNum = (LE(24) >>> 8) & 0xff, hdrLen = 32 + secNum * 16, pkt = 8 + hdrLen, hplen = 24 + pkt;
  const hp = new Uint8Array(hplen);
  hp[2] = 0x0c; hp[8] = pkt & 0xff; hp[9] = (pkt >> 8) & 0xff;
  hp[24] = 0x0d; hp[28] = pkt & 0xff; hp[29] = (pkt >> 8) & 0x3f;
  hp.set(fw.subarray(0, hdrLen), 32);
  hp[60] = 2020 & 0xff; hp[61] = (2020 >> 8) & 0xff;
  bulk(7, hp, 3000);
  let fr = 0; for (let c = 0; c < 400000; c++) { if (r32(0x1e0) & 4) { fr = 1; break; } }
  let ns = 0, body = hdrLen;
  for (let i = 0; i < secNum; i++) {
    const d1 = LE(32 + i * 16 + 4); let sz = d1 & 0xffffff; if (d1 & (1 << 28)) sz += 8; let pp = body, rem = sz;
    while (rem > 0) {
      const ch = rem > 2020 ? 2020 : rem; const b = new Uint8Array(24 + ch);
      b[2] = 0x1c; b[8] = ch & 0xff; b[9] = (ch >> 8) & 0xff; b.set(fw.subarray(pp, pp + ch), 24);
      bulk(7, b, 3000); ns++; pp += ch; rem -= ch;
    }
    body += sz;
  }
  let booted = 0; for (let c = 0; c < 400000; c++) { if (((r32(0x1e0) >>> 5) & 7) === 7) { booted = 1; break; } }
  P(`hwburst fwdl: armed=${armed} fwdlrdy=${fr} sections=${ns} STS${booted ? "=7 BOOTED" : "!=7"} 0x1e0=${hex(r32(0x1e0))}\n`);
  return booted ? 0 : -1;
}

// ================= config replay (kind 1/2/3/4 op stream) =================
function skip_to_after_fwdl(blob: Uint8Array, from: number, sz: number): number {
  let p = from, sawHdr = 0, inSec = 0;
  while (p < sz) {
    const k = blob[p];
    if (k === 1) { if (inSec) return p; const ln = blob[p + 7] | (blob[p + 8] << 8); p += 9 + ln; }
    else if (k === 2) { const ep = blob[p + 1], ln = blob[p + 2] | (blob[p + 3] << 8); if (ep === 7 && ln === 112) sawHdr = 1; else if (ep === 7 && sawHdr) inSec = 1; p += 4 + ln; }
    else if (k === 3) { if (inSec) return p; p += 5; }
    else if (k === 4) { if (inSec) return p; p += 9; }
    else return p;
  }
  return sawHdr ? p : -1;
}
function replay(blob: Uint8Array, startByte: number, fwpath: string) {
  const sz = blob.length; let p = startByte;
  const rbuf = new Uint8Array(4);
  let nw = 0, nr = 0, np = 0, nb = 0, ptmo = 0, fail = 0, ri = 0, bursts = 0;
  const u16 = () => { const v = blob[p] | (blob[p + 1] << 8); p += 2; return v; };
  const u32 = () => { const v = (blob[p] | (blob[p + 1] << 8) | (blob[p + 2] << 16) | (blob[p + 3] << 24)) >>> 0; p += 4; return v; };
  while (p < sz) {
    const kind = blob[p++]; ri++;
    if (kind === 1) {
      const brt = blob[p++], br = blob[p++], wv = u16(), wi = u16(), ln = u16();
      const addr = (wv | (wi << 16)) >>> 0;
      const v0 = ln >= 4 ? (blob[p] | (blob[p + 1] << 8) | (blob[p + 2] << 16) | (blob[p + 3] << 24)) >>> 0 : (ln >= 1 ? blob[p] : 0);
      if (addr === 0x2f0 && v0 === 0) {
        const endp = skip_to_after_fwdl(blob, p + ln, sz);
        if (endp >= 0) { const rc = hwburst_fwdl(fwpath); bursts++; P(`  [re-fwdl @op${ri}] rc=${rc} skip->${endp} 0x1e0=${hex(r32(0x1e0))}\n`); p = endp; continue; }
      }
      const data = blob.subarray(p, p + ln); p += ln;
      // register write over the control pipe (brt/br are 0x40/0x05 for writes)
      const b = new Uint8Array(data); // control() needs its own buffer length = ln
      controlRaw(brt, br, wv, wi, b); nw++;
    } else if (kind === 3) {
      const wv = u16(), wi = u16(); controlRaw(0xC0, 0x05, wv, wi, rbuf); nr++;
    } else if (kind === 4) {
      const wv = u16(), wi = u16(), val = u32(); const addr = (wv | (wi << 16)) >>> 0;
      if (addr === 0x1bff8) { let hit = 0; for (let c = 0; c < 4000; c++) { if ((r32(addr) & 0xff) === 0x55) { hit = 1; break; } } if (!hit) ptmo++; np++; }
      else { let hit = 0; for (let c = 0; c < 600; c++) { const rr = r32(addr); if (rr === val || (rr & val) === val) { hit = 1; break; } } if (!hit) ptmo++; np++; }
    } else if (kind === 2) {
      const ep = blob[p++], ln = u16();
      if (ep === 7 && ln !== 112) { // section run — send this section, then consecutive ep7 sections (p is at data)
        bulk(7, blob.subarray(p, p + ln), 3000); nb++; p += ln;
        while (p < sz && blob[p] === 2) { const e = blob[p + 1], l = blob[p + 2] | (blob[p + 3] << 8); if (e !== 7 || l === 112) break; p += 4; bulk(7, blob.subarray(p, p + l), 3000); nb++; p += l; }
        let booted = 0; for (let c = 0; c < 3000; c++) { if (((r32(0x1e0) >>> 5) & 7) === 7) { booted = 1; break; } }
        P(`  [burst @op${ri}] STS${booted ? "=7 BOOTED" : "!=7"} 0x1e0=${hex(r32(0x1e0))}\n`);
      } else if (ep === 7 && ln === 112) { // fwdl header
        for (let c = 0; c < 3000; c++) { if (r32(0x1e0) & 2) break; }
        bulk(7, blob.subarray(p, p + ln), 3000); p += ln;
        for (let c = 0; c < 3000; c++) { if (r32(0x1e0) & 4) break; }
        nb++;
      } else { bulk(ep, blob.subarray(p, p + ln), 3000); p += ln; nb++; }
    } else { P(`bad kind ${kind}\n`); break; }
    if (ri % 1000 === 0) P(`  ..${ri} (w=${nw} r=${nr} p=${np} b=${nb} ptmo=${ptmo}) BB8008=${hex(r32(BB(0x8008)))} 0x1e0=${hex(r32(0x1e0))}\n`);
  }
  P(`REPLAY done: ${ri} ops (w=${nw} r=${nr} p=${np} b=${nb} bursts=${bursts} ptmo=${ptmo} fail=${fail})\n`);
}
// Control passthrough for the replay. The captured config op stream is entirely 4-byte register reads/writes
// (kernel rtw89 reg access), so routing through the exported r32/w32 primitives is exact. If a future capture
// carried a non-4-byte control transfer, this would need the raw control() from ax56tx_hw exported instead.
function controlRaw(reqType: number, _req: number, wv: number, wi: number, data: Uint8Array) {
  if (reqType === 0x40) { const v = (data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24)) >>> 0; w32((wv | (wi << 16)) >>> 0, v); }
  else { const v = r32((wv | (wi << 16)) >>> 0); data[0] = v & 0xff; data[1] = (v >> 8) & 0xff; data[2] = (v >> 16) & 0xff; data[3] = (v >> 24) & 0xff; }
}

// ================= IQK chain (verbatim from hwdriver.c) =================
let g_band = 0, g_bw = 0, g_syn = 0;
function iqk_get_ch_info(path: number) {
  g_band = 0; g_bw = 0;
  const reg35c = bbr(0x35c, 0x00000c00);
  g_syn = (reg35c === 0x01) ? 1 : 0;
  bbm(0x9fe4, (0x000f << (path * 16)) >>> 0, g_band);
  bbm(0x9fe4, (0x00f0 << (path * 16)) >>> 0, g_bw);
  bbm(0x9fe4, (0xff00 << (path * 16)) >>> 0, 6);
  P(`  [get_ch_info p${path}] band=${g_band} bw=${g_bw} syn1to2=${g_syn}\n`);
}
const IQK_MACBB: number[][] = [
  [0x20fc, 0xffff0000, 0x00000303], [0x5864, 0x18000000, 0x00000003], [0x7864, 0x18000000, 0x00000003],
  [0x12b8, 0x40000000, 0x00000001], [0x32b8, 0x40000000, 0x00000001], [0x030c, 0xff000000, 0x00000013],
  [0x032c, 0xffff0000, 0x00000001], [0x12b8, 0x10000000, 0x00000001], [0x58c8, 0x01000000, 0x00000001],
  [0x78c8, 0x01000000, 0x00000001], [0x5864, 0xc0000000, 0x00000003], [0x7864, 0xc0000000, 0x00000003],
  [0x2008, 0x01ffffff, 0x01ffffff], [0x0c1c, 0x00000004, 0x00000001], [0x0700, 0x08000000, 0x00000001],
  [0x0c70, 0x000003ff, 0x000003ff], [0x0c60, 0x00000003, 0x00000003], [0x0c6c, 0x00000001, 0x00000001],
  [0x58ac, 0x08000000, 0x00000001], [0x78ac, 0x08000000, 0x00000001], [0x0c3c, 0x00000200, 0x00000001],
  [0x2344, 0x80000000, 0x00000001], [0x4490, 0x80000000, 0x00000001], [0x12a0, 0x00007000, 0x00000007],
  [0x12a0, 0x00008000, 0x00000001], [0x12a0, 0x00070000, 0x00000003], [0x12a0, 0x00080000, 0x00000001],
  [0x32a0, 0x00070000, 0x00000003], [0x32a0, 0x00080000, 0x00000001], [0x0700, 0x01000000, 0x00000001],
  [0x0700, 0x06000000, 0x00000002], [0x20fc, 0xffff0000, 0x00003333], [0x58f0, 0x00080000, 0x00000000],
  [0x78f0, 0x00080000, 0x00000000],
];
function iqk_macbb_setting() { for (const r of IQK_MACBB) bbm(r[0], r[1], r[2]); }
function iqk_preset(path: number) {
  bbm(0x8104 + (path << 8), 0x1, 0); bbm(0x8154 + (path << 8), 0x4, 0); wrf(path, 0x05, 0x1, 0x0);
  bbm(0x8008, 0xffffffff, 0x00000080); bbm(0x8080, 0xffffffff, 0x0); bbm(0x8088, 0xffffffff, 0x81ff010a);
  bbm(0x80d0, 0xffffffff, 0x00200000); bbm(0x8074, 0xffffffff, 0x80000000); bbm(0x81dc + (path << 8), 0xffffffff, 0x0);
}
function iqk_txclk_setting(path: number) { bbm(0x8120 + (path << 8), 0xffffffff, 0xce000a08); }
function lok_res_table(path: number, ibias: number) {
  wrf(path, 0xef, RFREG_MASK, 0x2); wrf(path, 0x33, RFREG_MASK, (g_band === 0) ? 0x0 : 0x1);
  wrf(path, 0x3f, RFREG_MASK, ibias); wrf(path, 0xef, RFREG_MASK, 0x0);
}
function iqk_txk_setting(path: number) {
  bbset(0x12b8 + (path << 13), (1 << 30) >>> 0);
  bbm(0x030c, 0xff000000, 0x1f); usleep(1); bbm(0x030c, 0xff000000, 0x13);
  bbm(0x032c, 0xffff0000, 0x0001); usleep(1); bbm(0x032c, 0xffff0000, 0x0041); usleep(1);
  bbm(0x20fc, 0xffff0000, 0x0303); bbm(0x20fc, 0xffff0000, 0x0000);
  wrf(path, 0x90, 0x3, 0x00); wrf(path, 0xde, (0x7f << 13) >>> 0, 0x3f); wrf(path, 0x51, (1 << 19) >>> 0, 0x0);
  wrf(path, 0x51, (1 << 11) >>> 0, 0x1); wrf(path, 0x52, (1 << 11) >>> 0, 0x1); wrf(path, 0x55, 0x1, 0x0);
  wrf(path, 0xef, 0x4, 0x1); wrf(path, 0xdf, 0x4, 0x0); wrf(path, 0x33, 0x3ff, 0x000);
  wrf(path, 0x09, RFREG_MASK, 0x80200); wrf(path, 0x08, RFREG_MASK, 0x80200);
  wrf(path, 0x00, RFREG_MASK, (0x403e0 | g_syn) >>> 0); usleep(1);
}
function iqk_one_shot(path: number, fine: number) {
  const rfc = (path === 0) ? 0x5864 : 0x7864;
  bbset(rfc, 0x20000000); bbm(0x802c, 0xfff, 0x009);
  const cmd = ((fine ? 0x208 : 0x108) | (1 << (4 + path))) >>> 0;
  bbm(0x8000, 0xffffffff, cmd + 1); bbset(0x80b0, (1 << 28) >>> 0); usleep(1);
  let done = 0, i = 0; for (; i < 120; i++) { if ((r32(BB(0xbff8)) & 0xff) === 0x55) { done = 1; break; } usleep(1); }
  bbclr(0x8010, 0xff); const rpt = r32(BB(0x8008)); bbclr(rfc, 0x20000000);
  P(`    [one_shot p${path} ${fine ? "FINE" : "COARSE"}] done=${done}(i=${i}) rpt=${hex(rpt)}\n`);
}
function iqk_lok(path: number): number {
  const itqt = (g_band === 0) ? 0x09 : 0x12;
  wrf(path, 0x56, 0xffff, (g_band === 0) ? 0xe5e0 : 0xe4e0);
  bbset(0x8034, 0x30); const rf0 = rrf(path, 0x00, RFREG_MASK); bbm(0x8020, 0xfffff, (rf0 | g_syn) >>> 0);
  bbclr(0x8124 + (path << 8), 0xf00); bbm(0x8154 + (path << 8), 0x100, 0x1); bbm(0x8154 + (path << 8), 0x8, 0x1);
  bbm(0x8154 + (path << 8), 0x3, 0x0); bbset(0x0c60, 0x2); bbclr(0x8010, 0xff);
  bbm(0x81cc + (path << 8), 0xffffffff, itqt); iqk_one_shot(path, 0); usleep(10000);
  bbm(0x81cc + (path << 8), 0xffffffff, itqt); iqk_one_shot(path, 1);
  const tmp = rrf(path, 0x58, RFREG_MASK); const ci = (tmp >> 15) & 0x1f, cq = (tmp >> 10) & 0x1f;
  const fail = (ci < 2 || ci > 0x1d || cq < 2 || cq > 0x1d) ? 1 : 0;
  P(`    [lok p${path}] TXMO=${hex(tmp)} i=${hex(ci)} q=${hex(cq)} fail=${fail}\n`);
  return fail;
}
const g_nb_txcfir = [0x40000000, 0x40000000], g_nb_rxcfir = [0x40000000, 0x40000000];
function txk_one_shot(path: number): number {
  const rfc = (path === 0) ? 0x5864 : 0x7864;
  bbclr(rfc, 0x20000000); bbm(0x802c, 0xfff, 0x025);
  const cmd = (0x808 | (1 << (4 + path))) >>> 0;
  bbm(0x8000, 0xffffffff, cmd + 1); bbset(0x80b0, (1 << 28) >>> 0); usleep(1);
  let done = 0; for (let i = 0; i < 400; i++) { if ((r32(BB(0xbff8)) & 0xff) === 0x55) { done = 1; break; } usleep(1); }
  bbclr(rfc, 0x20000000); return done;
}
function txk_group_sel(path: number) {
  const g_txgain = [0x60e8, 0x60f0, 0x61e8, 0x61ed], g_itqt = [0x09, 0x12, 0x12, 0x12], g_attsmxr = [0x0, 0x1, 0x1, 0x1];
  for (let gp = 0; gp < 4; gp++) {
    bbm(0x8148 + (path << 8), 0x1f, 0x08);
    wrf(path, 0x56, 0xffff, g_txgain[gp]);
    wrf(path, 0x51, (1 << 11) >>> 0, g_attsmxr[gp]);
    wrf(path, 0x52, (1 << 11) >>> 0, g_attsmxr[gp]);
    bbm(0x81cc + (path << 8), 0xffffffff, g_itqt[gp]);
    bbclr(0x8124 + (path << 8), (0xf << 8) >>> 0);
    bbset(0x8154 + (path << 8), (1 << 8) >>> 0);
    bbset(0x8154 + (path << 8), (1 << 3) >>> 0);
    bbm(0x8154 + (path << 8), 0x3, gp);
    bbclr(0x8010, 0xff);
    const done = txk_one_shot(path);
    P(`    [txk p${path} gp${gp}] done=${done}\n`);
  }
  bbm(0x8124 + (path << 8), (0xf << 8) >>> 0, 0x5);
  g_nb_txcfir[path] = 0x40000000;
}
function iqk_restore(path: number) {
  bbm(0x8138 + (path << 8), 0xffffffff, g_nb_txcfir[path]);
  bbm(0x813c + (path << 8), 0xffffffff, g_nb_rxcfir[path]);
  bbclr(0x8008, 0xffffffff); bbclr(0x8074, 0xffffffff); bbm(0x8088, 0xffffffff, 0x80000000);
  bbclr(0x80d0, 0xffffffff); bbclr(0x80e0, 0x1); bbm(0x8120 + (path << 8), 0xffffffff, 0x10010000);
  bbclr(0x8140 + (path << 8), (1 << 8) >>> 0); bbm(0x8150 + (path << 8), 0xffffffff, 0xe4e4e4e4);
  bbclr(0x8154 + (path << 8), (1 << 8) >>> 0); bbclr(0x81cc + (path << 8), 0x3f);
  bbm(0x81dc + (path << 8), 0xffffffff, 0x00000002);
  wrf(path, 0xef, (1 << 2) >>> 0, 0x0); wrf(path, 0xde, (0x7f << 13) >>> 0, 0x0); wrf(path, 0xef, (1 << 2) >>> 0, 0x0);
  wrf(path, 0x00, (0xf << 16) >>> 0, 0x3); wrf(path, 0x5c, (1 << 19) >>> 0, 0x0); wrf(path, 0x5e, (1 << 19) >>> 0, 0x0);
  wrf(path, 0x05, (1 << 0) >>> 0, 0x1);
}
const IQK_AFEBB_RESTORE: number[][] = [
  [0x20fc, 0xffff0000, 0x00000303], [0x12b8, 0x40000000, 0x0], [0x32b8, 0x40000000, 0x0],
  [0x5864, 0xc0000000, 0x0], [0x7864, 0xc0000000, 0x0], [0x2008, 0x01ffffff, 0x0],
  [0x0c1c, 0x00000004, 0x0], [0x0700, 0x08000000, 0x0], [0x0c70, 0x0000001f, 0x00000003],
  [0x0c70, 0x000003e0, 0x00000003], [0x12a0, 0x000ff000, 0x0], [0x32a0, 0x000ff000, 0x0],
  [0x0700, 0x07000000, 0x0], [0x5864, 0x20000000, 0x0], [0x7864, 0x20000000, 0x0],
  [0x0c3c, 0x00000200, 0x0], [0x2320, 0x00000001, 0x0], [0x20fc, 0xffff0000, 0x0],
  [0x58c8, 0x01000000, 0x0], [0x78c8, 0x01000000, 0x0],
];
function iqk_afebb_restore() { for (const r of IQK_AFEBB_RESTORE) bbm(r[0], r[1], r[2]); }
function iqk_chain(path: number) {
  iqk_get_ch_info(path); iqk_macbb_setting(); iqk_preset(path); iqk_txclk_setting(path);
  let ibias = 1, lokfail = 1;
  for (let t = 0; t < 3 && lokfail; t++) {
    lok_res_table(path, ibias++); iqk_txk_setting(path);
    if (t === 0) { wrf(path, 0x56, 0xffff, 0xe5e0); const g = rrf(path, 0x56, 0xffff); P(`    [gain-hold p${path}] RR_GAINTX=${hex(g)} ${g === 0xe5e0 ? "HELD" : "NOT held"}\n`); }
    lokfail = iqk_lok(path);
  }
  txk_group_sel(path);
  iqk_restore(path);
}

// ================= initcal (RCK + DACK), verbatim from initcal.c =================
const g_msbk: number[][][] = [[[], []], [[], []]];
const g_biask = [[0, 0], [0, 0]], g_dadck = [[0, 0], [0, 0]], g_addck = [[0, 0], [0, 0]];
function bbpoll(bba: number, bit: number, tries: number): number { for (let c = 0; c < tries; c++) { if (r32(BB(bba)) & bit) return 1; usleep(1); } return 0; }
function rck(path: number) {
  const rf5 = rrf(path, 0x05, RFREG_MASK);
  wrf(path, 0x05, 0x1, 0x0); wrf(path, 0x00, 0xf0000, 0x3); wrf(path, 0x1b, RFREG_MASK, 0x00240);
  let ok = 0; for (let c = 0; c < 40; c++) { if (rrf(path, 0x1c, 0x8)) { ok = 1; break; } usleep(2); }
  const rckv = rrf(path, 0x1b, 0x7c00); wrf(path, 0x1b, RFREG_MASK, rckv);
  wrf(path, 0x1d, 0x3e00, 0x4); wrf(path, 0xf0, 0x2, 0x1); wrf(path, 0xf0, 0x2, 0x0);
  wrf(path, 0x05, RFREG_MASK, rf5);
  P(`[RCK]S${path} ok=${ok} rckv=${hex(rckv)}\n`);
}
function addck_backup() {
  bbclr(0x12D8, 0x300); g_addck[0][0] = bbr(0x1E00, 0xffc00); g_addck[0][1] = bbr(0x1E00, 0x3ff);
  bbclr(0x32D8, 0x300); g_addck[1][0] = bbr(0x3E00, 0xffc00); g_addck[1][1] = bbr(0x3E00, 0x3ff);
}
function addck_reload() {
  bbm(0x12D4, 0x3ff0000, g_addck[0][0]); bbm(0x12D8, 0xf, g_addck[0][1] >> 6); bbm(0x12D4, 0xfc000000, g_addck[0][1] & 0x3f); bbset(0x12D8, 0x30);
  bbm(0x32D4, 0x3ff0000, g_addck[1][0]); bbm(0x32D8, 0xf, g_addck[1][1] >> 6); bbm(0x32D4, 0xfc000000, g_addck[1][1] & 0x3f); bbset(0x32D8, 0x30);
}
function addck() {
  T.TBL_rfk_addck_reset_defs_a(); T.TBL_rfk_check_addc_defs_a(); T.TBL_rfk_addck_trigger_defs_a();
  const a = bbpoll(0x1e00, 0x1, 3000); T.TBL_rfk_check_addc_defs_a(); T.TBL_rfk_addck_restore_defs_a();
  T.TBL_rfk_addck_reset_defs_b(); T.TBL_rfk_check_addc_defs_b(); T.TBL_rfk_addck_trigger_defs_b();
  const b = bbpoll(0x3e00, 0x1, 3000); T.TBL_rfk_check_addc_defs_b(); T.TBL_rfk_addck_restore_defs_b();
  P(`[ADDCK]s0=${a} s1=${b}\n`);
}
function dack_backup_s0() {
  bbset(0x5E00, 0x8); bbset(0x5E50, 0x8); bbset(0x12B8, 0x40000000);
  for (let i = 0; i < 16; i++) { bbm(0x5E00, 0xf0000000, i); g_msbk[0][0][i] = bbr(0x5E44, 0xff00); bbm(0x5E50, 0xf0000000, i); g_msbk[0][1][i] = bbr(0x5E94, 0xff00); }
  g_biask[0][0] = bbr(0x5E30, 0x3ff000); g_biask[0][1] = bbr(0x5E80, 0x3ff000);
  g_dadck[0][0] = (bbr(0x5E48, 0xff00) - 8) & 0xff; g_dadck[0][1] = (bbr(0x5E98, 0xff00) - 8) & 0xff;
}
function dack_backup_s1() {
  bbset(0x7E00, 0x8); bbset(0x7E50, 0x8); bbset(0x32B8, 0x40000000);
  for (let i = 0; i < 16; i++) { bbm(0x7E00, 0xf0000000, i); g_msbk[1][0][i] = bbr(0x7E44, 0xff00); bbm(0x7E50, 0xf0000000, i); g_msbk[1][1][i] = bbr(0x7E94, 0xff00); }
  g_biask[1][0] = bbr(0x7E30, 0x3ff000); g_biask[1][1] = bbr(0x7E80, 0x3ff000);
  g_dadck[1][0] = (bbr(0x7E48, 0xff00) - 8) & 0xff; g_dadck[1][1] = (bbr(0x7E98, 0xff00) - 8) & 0xff;
}
function dack_reload_by_path(path: number, idx: number) {
  const off = (idx ? 0x50 : 0) + (path ? 0x2000 : 0); let tmp: number;
  tmp = 0; for (let i = 0; i < 4; i++) tmp |= g_msbk[path][idx][i + 12] << (i * 8); w32(BB(0x5e14 + off), tmp >>> 0);
  tmp = 0; for (let i = 0; i < 4; i++) tmp |= g_msbk[path][idx][i + 8] << (i * 8); w32(BB(0x5e18 + off), tmp >>> 0);
  tmp = 0; for (let i = 0; i < 4; i++) tmp |= g_msbk[path][idx][i + 4] << (i * 8); w32(BB(0x5e1c + off), tmp >>> 0);
  tmp = 0; for (let i = 0; i < 4; i++) tmp |= g_msbk[path][idx][i] << (i * 8); w32(BB(0x5e20 + off), tmp >>> 0);
  tmp = ((g_biask[path][idx] << 22) | (g_dadck[path][idx] << 14)) >>> 0; w32(BB(0x5e24 + off), tmp);
}
function dack_reload(path: number) { dack_reload_by_path(path, 0); dack_reload_by_path(path, 1); if (path === 0) T.TBL_rfk_dack_reload_defs_a(); else T.TBL_rfk_dack_reload_defs_b(); }
function dack_s0() {
  T.TBL_rfk_dack_defs_f_a(); const m0 = bbpoll(0x5e28, 0x8000, 3000) & bbpoll(0x5e78, 0x8000, 3000);
  T.TBL_rfk_dack_defs_m_a(); const d0 = bbpoll(0x5e48, 0x20000, 3000) & bbpoll(0x5e98, 0x20000, 3000);
  T.TBL_rfk_dack_defs_r_a(); dack_backup_s0(); dack_reload(0); bbclr(0x12B8, 0x40000000);
  P(`[DACK]S0 msbk=${m0} dad=${d0} m0=${hex(g_msbk[0][0][0])} bias=${hex(g_biask[0][0])} dadck=${hex(g_dadck[0][0])}\n`);
}
function dack_s1() {
  T.TBL_rfk_dack_defs_f_b(); const m1 = bbpoll(0x7e28, 0x8000, 3000) & bbpoll(0x7e78, 0x8000, 3000);
  T.TBL_rfk_dack_defs_m_b(); const d1 = bbpoll(0x7e48, 0x20000, 3000) & bbpoll(0x7e98, 0x20000, 3000);
  T.TBL_rfk_dack_defs_r_b(); dack_backup_s1(); dack_reload(1); bbclr(0x32B8, 0x40000000);
  P(`[DACK]S1 msbk=${m1} dad=${d1} m0=${hex(g_msbk[1][0][0])} bias=${hex(g_biask[1][0])} dadck=${hex(g_dadck[1][0])}\n`);
}
function initcal() {
  P("[INITCAL] live RCK+DACK\n");
  rck(0); rck(1);
  const rf0 = rrf(0, 0x00, RFREG_MASK), rf1 = rrf(1, 0x00, RFREG_MASK);
  T.TBL_rfk_afe_init_defs();
  wrf(0, 0x05, 0x1, 0x0); wrf(1, 0x05, 0x1, 0x0); wrf(0, 0x00, RFREG_MASK, 0x30001); wrf(1, 0x00, RFREG_MASK, 0x30001);
  addck(); addck_backup(); addck_reload();
  wrf(0, 0x00, RFREG_MASK, 0x40001); wrf(1, 0x00, RFREG_MASK, 0x40001); wrf(0, 0x01, RFREG_MASK, 0x0); wrf(1, 0x01, RFREG_MASK, 0x0);
  dack_s0(); dack_s1();
  wrf(0, 0x00, RFREG_MASK, rf0); wrf(1, 0x00, RFREG_MASK, rf1); wrf(0, 0x05, 0x1, 0x1); wrf(1, 0x05, 0x1, 0x1);
  P(`[INITCAL] done 0xF0=${hex(r32(0xf0))}\n`);
}

// ================= TSSI (verbatim from tssi_all.c logic) =================
function tssi_tmeter(path: number) {
  const TM = path ? 0x7810 : 0x5810, RC = path ? 0x7864 : 0x5864, BASE = path ? 0x7c00 : 0x5c00;
  bbm(TM, 0x10000, 0); bbm(TM, 0x1000000, 1); bbm(TM, 0xfc00, 32); bbm(RC, 0x3f00000, 32);
  for (let i = 0; i < 64; i += 4) w32(BB(BASE + i), 0);
  bbm(RC, 0x4000000, 1); bbm(RC, 0x4000000, 0);
}
function tssi_live() {
  T.TBL_tssi_disable_defs();
  for (let path = 0; path < 2; path++) {
    wrf(path, 0x7f, 0x2, 0x1);
    T.TBL_tssi_sys_defs(); T.TBL_tssi_sys_defs_2g();
    if (path === 0) T.TBL_tssi_txpwr_ctrl_bb_defs_a(); else T.TBL_tssi_txpwr_ctrl_bb_defs_b();
    T.TBL_tssi_txpwr_ctrl_bb_defs_2g();
    if (path === 0) T.TBL_tssi_txpwr_ctrl_bb_he_tb_defs_a(); else T.TBL_tssi_txpwr_ctrl_bb_he_tb_defs_b();
    if (path === 0) T.TBL_tssi_dck_defs_a(); else T.TBL_tssi_dck_defs_b();
    tssi_tmeter(path);
    if (path === 0) T.TBL_tssi_dac_gain_tbl_defs_a(); else T.TBL_tssi_dac_gain_tbl_defs_b();
    if (path === 0) T.TBL_tssi_slope_cal_org_defs_a(); else T.TBL_tssi_slope_cal_org_defs_b();
    if (path === 0) T.TBL_tssi_rf_gap_tbl_defs_a(); else T.TBL_tssi_rf_gap_tbl_defs_b();
    if (path === 0) T.TBL_tssi_slope_defs_a(); else T.TBL_tssi_slope_defs_b();
    if (path === 0) T.TBL_tssi_pak_defs_a_2g(); else T.TBL_tssi_pak_defs_b_2g();
  }
  for (let path = 0; path < 2; path++) {
    if (path === 0) T.TBL_tssi_track_defs_a(); else T.TBL_tssi_track_defs_b();
    if (path === 0) T.TBL_tssi_txagc_ofst_mv_avg_defs_a(); else T.TBL_tssi_txagc_ofst_mv_avg_defs_b();
    if (path === 0) T.TBL_tssi_enable_defs_a(); else T.TBL_tssi_enable_defs_b();
  }
}

// ================= DPK (verbatim from dpk_live.c) =================
const DMASK = 0xffffffff;
const DPK_BKBB = [0x2344, 0x58f0, 0x78f0], DPK_BKRF = [0xef, 0xde, 0x00, 0x1e, 0x02, 0x85, 0x90, 0x05];
const dpk_bkbb = [0, 0, 0], dpk_bkrf = [[0, 0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0, 0]], dpk_path_ok = [0, 0];
const sx12 = (v: number) => { v &= 0xfff; return (v & 0x800) ? v - 0x1000 : v; };
function dpk_one_shot(path: number, id: number): number {
  const cmd = ((id << 8) | (0x19 + (path << 4))) & 0xffff;
  bbclr(0x8010, 0xff); bbm(0x8000, DMASK, cmd); bbset(0x80b0, 0x10000000);
  let done = 0, i = 0; for (; i < 400; i++) { if ((r32(BB(0xbff8)) & 0xff) === 0x55) { done = 1; break; } usleep(10); }
  bbclr(0x8010, 0xff); P(`      [dpk_os p${path} id=${hex(id)}] done=${done}(i=${i})\n`); return done ? 0 : 1;
}
function dpk_rx_dck(path: number) {
  wrf(path, 0x8f, 0xc00, 0x3); wrf(path, 0x94, 0xfc, 0x3f); wrf(path, 0x93, 0x8, 0x0);
  wrf(path, 0x92, 0x1, 0x0); wrf(path, 0x92, 0x1, 0x1); usleep(600); wrf(path, 0x92, 0x1, 0x0);
}
function dpk_rf_setting(path: number) {
  wrf(path, 0x00, 0xfffe0, 0x280b); wrf(path, 0x83, 0x7, 0x0); wrf(path, 0x83, 0xf0, 0x4);
  wrf(path, 0x9f, 0x18, 0x0); wrf(path, 0xde, 0x4, 0x1); wrf(path, 0x1a, 0x7000, g_bw + 1); wrf(path, 0x1a, 0xc00, 0x0);
}
function dpk_set_tx_pwr(path: number): number { wrf(path, 0x01, RFREG_MASK, 0x38); return 0x38; }
function dpk_kip_setting(path: number, kidx: number) {
  bbm(0x8008, DMASK, 0x00000080); bbm(0x808c, DMASK, 0x00093f3f); bbm(0x8088, DMASK, 0x807f030a);
  bbm(0x8120 + (path << 8), DMASK, 0xce000a08); bbm(0x80b8, 0x7000, 0x2); bbm(0x8000, 0x6, path);
  bbm(0x81ac + (path << 8) + (kidx << 2), DMASK, 0x003f2e2e); bbm(0x81bc + (path << 8) + (kidx << 2), DMASK, 0x005b5b5b);
}
function dpk_kip_restore(path: number) {
  bbclr(0x8008, DMASK); bbm(0x8088, DMASK, 0x80000000); bbm(0x8120 + (path << 8), DMASK, 0x10010000); bbclr(0x808c, DMASK);
}
function dpk_manual_txcfir(path: number, is_manual: number) {
  if (is_manual) {
    bbm(0x8140 + (path << 8), 0x100, 0x1);
    const pad = rrf(path, 0x56, 0x3e0); bbm(0x8144 + (path << 8), 0x1f, pad);
    const txbb = rrf(path, 0x56, 0x1f); bbm(0x8144 + (path << 8), 0x1f00, txbb);
    P(`      [dpk txcfir p${path}] pad=${hex(pad)} txbb=${hex(txbb)}\n`);
    bbm(0x81dc + (path << 8), 0x3, 0x1); bbclr(0x81dc + (path << 8), 0x3); bbm(0x81dc + (path << 8), 0x2, 0x1);
  } else bbclr(0x8140 + (path << 8), 0x100);
}
function dpk_bypass_rxcfir(path: number, on: number) {
  if (on) { bbm(0x813c + (path << 8), 0x4, 0x1); bbm(0x813c + (path << 8), 0x1, 0x1); }
  else { bbclr(0x813c + (path << 8), 0x4); bbclr(0x813c + (path << 8), 0x1); }
}
function dpk_table_select(path: number, kidx: number, gain: number) { bbm(0x81ac + (path << 8), 0xff000000, (0x80 + kidx * 0x20 + gain * 0x10) >>> 0); }
function dpk_tpg_sel(_path: number, _kidx: number) { bbm(0x806c, 0x6, 0x1); }
function dpk_set_mdpd_para(order: number) { bbm(0x80a0, 0x3, order); bbm(0x80a0, 0x1f00, 0x3); bbm(0x8070, 0xf0000000, 0x1); }
function dpk_lbk_rxiqk(path: number) {
  const cur = rrf(path, 0x00, 0x3e0);
  T.TBL_rfk_dpk_lbk_rxiqk_defs_f();
  wrf(path, 0x00, 0xf0000, 0xc); wrf(path, 0x20, 0x20, 0x1); wrf(path, 0x80, 0x30000, 0x2);
  wrf(path, 0x1f, RFREG_MASK, rrf(path, 0x18, RFREG_MASK)); wrf(path, 0x1e, 0x3f, 0x13);
  wrf(path, 0x1e, 0x80000, 0x0); wrf(path, 0x1e, 0x80000, 0x1); usleep(70);
  wrf(path, 0x8d, 0x1f00, 0x1f);
  if (cur <= 0xa) wrf(path, 0x8d, 0x6000, 0x3); else if (cur <= 0x10) wrf(path, 0x8d, 0x6000, 0x1); else wrf(path, 0x8d, 0x6000, 0x0);
  bbm(0x802c, 0x0fff0000, 0x11); dpk_one_shot(path, 0x06);
  wrf(path, 0x20, 0x20, 0x0); wrf(path, 0x80, 0x30000, 0x0); wrf(path, 0x1e, 0x80000, 0x0); wrf(path, 0x00, 0xf0000, 0x5);
  T.TBL_rfk_dpk_lbk_rxiqk_defs_r();
}
function dpk_sync_check(_path: number): number {
  bbclr(0x80d4, 0x3f0000);
  const corr_idx = bbr(0x80fc, 0xff), corr_val = bbr(0x80fc, 0xff00);
  bbm(0x80d4, 0x3f0000, 0x9);
  const dc_i = Math.abs(sx12(bbr(0x80fc, 0x0fff0000))), dc_q = Math.abs(sx12(bbr(0x80fc, 0xfff)));
  P(`      [dpk sync] corr_idx=${corr_idx} corr_val=${corr_val} dc_i=${dc_i} dc_q=${dc_q}\n`);
  return (dc_i > 200 || dc_q > 200 || corr_val < 130) ? 1 : 0;
}
function dpk_sync(path: number): number { dpk_tpg_sel(path, 0); dpk_one_shot(path, 0x10); return dpk_sync_check(path); }
function dpk_dgain_read(): number { bbclr(0x80d4, 0x3f0000); bbr(0x80fc, 0x40000000); return bbr(0x80fc, 0x0fff0000); }
function dpk_dgain_mapping(d: number): number {
  if (d >= 0x783) return 6; if (d >= 0x551) return 3; if (d >= 0x3c4) return 0;
  if (d >= 0x2aa) return -3; if (d >= 0x1e3) return -6; if (d >= 0x156) return -9; if (d <= 0x155) return -12; return 0;
}
function dpk_gainloss_read(): number { bbm(0x80d4, 0x3f0000, 0x6); bbm(0x80bc, 0x4000, 0x1); return bbr(0x80fc, 0xf0); }
function dpk_gainloss(path: number) { dpk_table_select(path, 0, 1); dpk_one_shot(path, 0x13); }
function dpk_set_offset(path: number, goff: number): number {
  let txagc = rrf(path, 0x01, RFREG_MASK);
  if (txagc - goff < 0x2e) txagc = 0x2e; else if (txagc - goff > 0x3f) txagc = 0x3f; else txagc = txagc - goff;
  wrf(path, 0x01, RFREG_MASK, txagc); return txagc;
}
function dpk_pas_read(): number {
  T.TBL_rfk_dpk_pas_read_defs();
  bbm(0x80c0, 0xff000000, 0x00);
  const v1i = Math.abs(sx12(bbr(0x80fc, 0xffff0000))), v1q = Math.abs(sx12(bbr(0x80fc, 0xffff)));
  bbm(0x80c0, 0xff000000, 0x1f);
  const v2i = Math.abs(sx12(bbr(0x80fc, 0xffff0000))), v2q = Math.abs(sx12(bbr(0x80fc, 0xffff)));
  const a = v1i * v1i + v1q * v1q, b = v2i * v2i + v2q * v2q;
  return (a >= Math.floor(b * 8 / 5)) ? 1 : 0;
}
function dpk_agc(path: number, _kidx: number, init_txagc: number): number {
  let tmp_txagc = init_txagc, tmp_rxbb = 0, tmp_gl = 0, agc_cnt = 0, limited = 0, off = 0, dgain = 0, step = 0, goout = 0;
  do {
    switch (step) {
      case 0:
        if (dpk_sync(path)) { tmp_txagc = 0xff; goout = 1; break; }
        dgain = dpk_dgain_read(); step = limited ? 2 : 1; break;
      case 1:
        tmp_rxbb = rrf(path, 0x00, 0x3e0); off = dpk_dgain_mapping(dgain);
        if (tmp_rxbb + off > 0x1f) { tmp_rxbb = 0x1f; limited = 1; }
        else if (tmp_rxbb + off < 0) { tmp_rxbb = 0; limited = 1; }
        else tmp_rxbb = tmp_rxbb + off;
        wrf(path, 0x00, 0x3e0, tmp_rxbb);
        if (off !== 0 || agc_cnt === 0) dpk_bypass_rxcfir(path, 1);
        step = (dgain > 1922 || dgain < 342) ? 0 : 2; agc_cnt++; break;
      case 2:
        dpk_gainloss(path); tmp_gl = dpk_gainloss_read();
        if ((tmp_gl === 0 && dpk_pas_read()) || tmp_gl > 7) step = 3;
        else if (tmp_gl === 0) step = 4; else step = 5; break;
      case 3:
        if (tmp_txagc === 0x2e) { goout = 1; } else tmp_txagc = dpk_set_offset(path, 3);
        step = 2; agc_cnt++; break;
      case 4:
        if (tmp_txagc === 0x3f) { goout = 1; } else tmp_txagc = dpk_set_offset(path, -2);
        step = 2; agc_cnt++; break;
      case 5:
        tmp_txagc = dpk_set_offset(path, tmp_gl); goout = 1; agc_cnt++; break;
      default: goout = 1; break;
    }
  } while (!goout && agc_cnt < 6);
  P(`      [dpk_agc p${path}] txagc=${hex(tmp_txagc)} rxbb=${hex(tmp_rxbb)} (cnt=${agc_cnt})\n`);
  return tmp_txagc;
}
function dpk_idl_mpa(path: number, kidx: number) { dpk_set_mdpd_para(0); dpk_table_select(path, kidx, 1); dpk_one_shot(path, 0x11); }
function dpk_fill_result(path: number, kidx: number, gain: number, txagc: number) {
  bbm(0x8104 + (path << 8), 0x100, kidx);
  bbm(0x81c4 + (path << 8), (0x3f << ((gain << 3) + (kidx << 4))) >>> 0, txagc);
  bbm(0x81b4 + (path << 8) + (kidx << 2), (0x1ff << (gain << 4)) >>> 0, 0x78);
  bbm(0x81dc + (path << 8), 0x10000, 0x1); bbclr(0x81dc + (path << 8), 0x10000);
  bbm(0x81bc + (path << 8) + (kidx << 2), DMASK, 0x065b5b5b);
  bbclr(0x81a0 + (path << 8), DMASK); bbclr(0x8070, 0x80000000);
}
function dpk_onoff(path: number, off: number) {
  const val = (!off && dpk_path_ok[path]) ? 1 : 0;
  bbm(0x81bc + (path << 8), 0xff000000, (0x6 | val) >>> 0);
}
function dpk_main(path: number): number {
  const kidx = 0; let txagc: number, is_fail = 0;
  wrf(path, 0x05, 0x1, 0x0);
  txagc = dpk_set_tx_pwr(path);
  dpk_rf_setting(path); dpk_rx_dck(path); dpk_kip_setting(path, kidx); dpk_manual_txcfir(path, 1);
  txagc = dpk_agc(path, kidx, txagc); if (txagc === 0xff) is_fail = 1;
  dpk_idl_mpa(path, kidx); wrf(path, 0x00, 0xf0000, 0x3); dpk_fill_result(path, kidx, 1, txagc); dpk_manual_txcfir(path, 0);
  dpk_path_ok[path] = is_fail ? 0 : 1;
  P(`    [dpk_main p${path}] txagc=${hex(txagc)} ${is_fail ? "CHECK" : "SUCCESS"}\n`);
  return is_fail;
}
function dpk_tssi_pause(path: number, pause: number) { bbm(0x5818 + (path << 13), 0x40000000, pause); }
function dpk_live_run() {
  P(`  [DPKLIVE] start (chip 0xF0=${hex(r32(0xf0))})\n`);
  for (let i = 0; i < 3; i++) dpk_bkbb[i] = r32(BB(DPK_BKBB[i]));
  for (let p = 0; p < 2; p++) { dpk_tssi_pause(p, 1); for (let i = 0; i < 8; i++) dpk_bkrf[p][i] = rrf(p, DPK_BKRF[i], RFREG_MASK); }
  T.TBL_rfk_dpk_bb_afe_s_defs_ab();
  for (let p = 0; p < 2; p++) { void dpk_lbk_rxiqk; const f = dpk_main(p); dpk_onoff(p, f); }
  T.TBL_rfk_dpk_bb_afe_r_defs_ab();
  for (let i = 0; i < 3; i++) w32(BB(DPK_BKBB[i]), dpk_bkbb[i]);
  for (let p = 0; p < 2; p++) { dpk_kip_restore(p); for (let i = 0; i < 8; i++) wrf(p, DPK_BKRF[i], RFREG_MASK, dpk_bkrf[p][i]); dpk_tssi_pause(p, 0); }
  P(`  [DPKLIVE] done: chip 0xF0=${hex(r32(0xf0))} 0x1e0=${hex(r32(0x1e0))} path_ok=${dpk_path_ok[0]}/${dpk_path_ok[1]}\n`);
}

// ================= inject (bulk-OUT EP5, [len:2LE][data] packets) =================
function inject(path: string, reps: number) {
  const f = Deno.readFileSync(path); let q = 0, ok = 0, err = 0, tot = 0;
  P(`INJECT: EP5, ${f.length} bytes, ${reps} reps\n`);
  for (let rep = 0; rep < reps; rep++) {
    q = 0;
    while (q + 2 <= f.length) {
      const ln = f[q] | (f[q + 1] << 8); q += 2; if (q + ln > f.length) break;
      const rc = bulk(5, f.subarray(q, q + ln), 1000); if (rc >= 0) ok++; else { err++; if (err <= 3) P(`  inject rc=${rc}\n`); }
      q += ln; tot++;
    }
  }
  P(`INJECT done: ${tot} packets, ok=${ok} err=${err} 0x1e0=${hex(r32(0x1e0))} 0xF0=${hex(r32(0xf0))}\n`);
}

// ================= main =================
function main() {
  if (!claim()) { P("claim failed\n"); Deno.exit(1); }
  P(`entry 0x1e0=${hex(r32(0x1e0))} 0xF0=${hex(r32(0xf0))}\n`);
  const fw = env("FW") || "/tmp/fw_cut2_nic.bin";
  const blobPath = env("BLOB");
  if (blobPath) {
    const blob = Deno.readFileSync(blobPath);
    const start = Number(env("START") || "0");
    if (env("HB")) { P("hwburst init+fwdl...\n"); if (hwburst_fwdl(fw) !== 0) { P(`*** FWDL DID NOT BOOT 0x1e0=${hex(r32(0x1e0))} — cold replug needed\n`); Deno.exit(2); } }
    P(`  [DIAG a] after fwdl: BB(0x8008)=${hex(r32(BB(0x8008)))} BB(0x1c060)=${hex(r32(BB(0x1c060)))} 0xF0=${hex(r32(0xf0))} 0x1e0=${hex(r32(0x1e0))}\n`);
    replay(blob, start, fw);
    P(`  [DIAG b] after replay: BB(0x8008)=${hex(r32(BB(0x8008)))} BB(0x1c060)=${hex(r32(BB(0x1c060)))} 0xF0=${hex(r32(0xf0))} 0x1e0=${hex(r32(0x1e0))}\n`);
  }
  if (env("INITCAL")) initcal();
  if (env("IQKCHAIN")) { P(`IQKCHAIN: live IQK (chip 0xF0=${hex(r32(0xf0))})\n`); iqk_chain(0); iqk_chain(1); iqk_afebb_restore(); P(`IQKCHAIN done 0xF0=${hex(r32(0xf0))}\n`); }
  if (env("TSSILIVE")) { P(`TSSILIVE (chip 0xF0=${hex(r32(0xf0))})\n`); tssi_live(); P(`TSSILIVE done 0xF0=${hex(r32(0xf0))}\n`); }
  if (env("DPKLIVE")) dpk_live_run();
  const injf = env("INJECT");
  if (injf) inject(injf, Number(env("INJECT_REPS") || "1"));
  try { logFd?.close(); } catch { /* */ }
}
main();
