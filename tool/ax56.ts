// ax56.ts — userspace control core for RTL8852AU (ASUS USB-AX56), no root.
// Runs as a termux-usb callback: argv[0] = inherited usbfs fd. Action via AX56_* env.
// PROVEN primitives only: identify, modeswitch (SCSI eject), register read/write.
// Monitor/scan/inject are NOT here yet — they need the fw-download + monitor port
// (see RESEARCH.md). This file is the honest, working slice.

const fd = Number(Deno.args[0]);
const action = Deno.env.get("AX56_ACTION") ?? "id";

const libc = Deno.dlopen("libc.so.6", {
  ioctl: { parameters: ["i32", "u64", "buffer"], result: "i32" },
  pread: { parameters: ["i32", "buffer", "usize", "i64"], result: "isize" },
  __errno_location: { parameters: [], result: "pointer" },
});
const errno = () => new Deno.UnsafePointerView(libc.symbols.__errno_location()!).getInt32();

const IOCTL = {
  CLAIM: 0x8004550Fn,
  DISCONNECT_CLAIM: 0x8108551Bn,
  CONTROL: 0xC0185500n,
  BULK: 0xC0185502n,
} as const;

function claim(): boolean {
  const iface = new Uint8Array(4);
  if (libc.symbols.ioctl(fd, IOCTL.CLAIM, iface) >= 0) return true;
  const dc = new Uint8Array(264);
  new DataView(dc.buffer).setUint32(4, 2, true); // unconditional disconnect+claim
  return libc.symbols.ioctl(fd, IOCTL.DISCONNECT_CLAIM, dc) >= 0;
}

function control(reqType: number, req: number, val: number, idx: number, data: Uint8Array, timeout = 1000): number {
  const r = new Uint8Array(24);
  const dv = new DataView(r.buffer);
  dv.setUint8(0, reqType);
  dv.setUint8(1, req);
  dv.setUint16(2, val, true);
  dv.setUint16(4, idx, true);
  dv.setUint16(6, data.length, true);
  dv.setUint32(8, timeout, true);
  dv.setBigUint64(16, BigInt(Deno.UnsafePointer.value(Deno.UnsafePointer.of(data))), true);
  const rc = libc.symbols.ioctl(fd, IOCTL.CONTROL, r);
  return rc < 0 ? -errno() : rc;
}

function bulk(ep: number, buf: Uint8Array, timeout = 2000): number {
  const r = new Uint8Array(24);
  const dv = new DataView(r.buffer);
  dv.setUint32(0, ep, true);
  dv.setUint32(4, buf.length, true);
  dv.setUint32(8, timeout, true);
  dv.setBigUint64(16, BigInt(Deno.UnsafePointer.value(Deno.UnsafePointer.of(buf))), true);
  const rc = libc.symbols.ioctl(fd, IOCTL.BULK, r);
  return rc < 0 ? -errno() : rc;
}

// --- rtw89 vendor register access (control 0xC0/0x40, bRequest 0x05) ---
const REG_REQ = 0x05;
function rtwRead(addr: number, len = 4): Uint8Array | null {
  const d = new Uint8Array(len);
  return control(0xC0, REG_REQ, addr, 0, d) === len ? d : null;
}
function rtwWrite(addr: number, value: number, len = 4): boolean {
  const d = new Uint8Array(len);
  new DataView(d.buffer).setUint32(0, value >>> 0, true);
  return control(0x40, REG_REQ, addr, 0, d.subarray(0, len)) >= 0;
}
const u32 = (b: Uint8Array) => new DataView(b.buffer, b.byteOffset).getUint32(0, true);

// --- actions ---
const enc = new TextEncoder();
const out = (s: string) => Deno.stdout.writeSync(enc.encode(s));

if (action === "id") {
  const b = new Uint8Array(18);
  const n = Number(libc.symbols.pread(fd, b, 18n, 0n));
  if (n < 18) { out("id: short read\n"); Deno.exit(1); }
  const h = (i: number) => b[i].toString(16).padStart(2, "0");
  const le = (i: number) => h(i + 1) + h(i);
  out(`vid=${le(8)} pid=${le(10)} usb=${le(2)} class=${h(4)}/${h(5)}/${h(6)} numCfg=${b[17]}\n`);
} else if (action === "reg") {
  if (!claim()) { out("reg: claim failed errno " + errno() + "\n"); Deno.exit(1); }
  const start = Number(Deno.env.get("AX56_ADDR") ?? "0");
  const count = Number(Deno.env.get("AX56_COUNT") ?? "16");
  for (let i = 0; i < count; i++) {
    const addr = start + i * 4;
    const b = rtwRead(addr);
    if (!b) { out(`  ${addr.toString(16).padStart(4, "0")}: <err>\n`); break; }
    if (i % 4 === 0) out(`  ${addr.toString(16).padStart(4, "0")}:`);
    out(` ${u32(b).toString(16).padStart(8, "0")}`);
    if (i % 4 === 3) out("\n");
  }
  out("\n");
} else if (action === "regw") {
  if (!claim()) { out("regw: claim failed\n"); Deno.exit(1); }
  const addr = Number(Deno.env.get("AX56_ADDR"));
  const val = Number(Deno.env.get("AX56_VAL"));
  const before = rtwRead(addr);
  const ok = rtwWrite(addr, val);
  const after = rtwRead(addr);
  out(`  0x${addr.toString(16)}: ${before ? u32(before).toString(16) : "?"} -> write ${ok ? "ok" : "FAIL"} -> ${after ? u32(after).toString(16) : "?"}\n`);
} else if (action === "switch") {
  if (!claim()) { out("switch: claim failed\n"); Deno.exit(1); }
  const MSG = [
    "5553424387654321000000000000061e000000000000000000000000000000",
    "5553424397654321000000000000061b000000020000000000000000000000",
  ];
  for (const m of MSG) {
    const cbw = Uint8Array.from(m.match(/../g)!.map((x) => parseInt(x, 16)));
    const sent = bulk(0x05, cbw);
    const csw = new Uint8Array(13);
    if (sent > 0) bulk(0x84, csw);
    out(`  eject msg sent=${sent}\n`);
  }
  out("  storage ejected — device re-enumerates as 0b05:1997\n");
} else {
  out(`unknown action: ${action}\n`);
  Deno.exit(2);
}
