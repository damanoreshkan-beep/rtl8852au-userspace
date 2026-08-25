// ax56tx_hw.ts — usbfs transport (termux-usb fd) + register primitives for the RTL8852AU TX chain.
// Faithful Deno-FFI port of hwdriver.c's r32/w32/wrf/rrf/bbm/bbr/bbset/bbclr over the ax56.ts ioctl transport.
// argv[0] = inherited usbfs fd (from `termux-usb -r -e`). Synchronous, like ax56.ts.

const fd = Number(Deno.args[0]);

const libc = Deno.dlopen("libc.so.6", {
  ioctl: { parameters: ["i32", "u64", "buffer"], result: "i32" },
  __errno_location: { parameters: [], result: "pointer" },
});
const errno = () => new Deno.UnsafePointerView(libc.symbols.__errno_location()!).getInt32();

const IOCTL = {
  CLAIM: 0x8004550Fn,
  DISCONNECT_CLAIM: 0x8108551Bn,
  CONTROL: 0xC0185500n,
  BULK: 0xC0185502n,
} as const;

const ptr = (b: Uint8Array) => BigInt(Deno.UnsafePointer.value(Deno.UnsafePointer.of(b)));

export function claim(): boolean {
  const iface = new Uint8Array(4);
  if (libc.symbols.ioctl(fd, IOCTL.CLAIM, iface) >= 0) return true;
  const dc = new Uint8Array(264);
  new DataView(dc.buffer).setUint32(4, 2, true); // unconditional disconnect + claim
  return libc.symbols.ioctl(fd, IOCTL.DISCONNECT_CLAIM, dc) >= 0;
}

// usbdevfs control: bmRequestType, bRequest, wValue, wIndex, wLength, timeout, data*
function control(reqType: number, req: number, val: number, idx: number, data: Uint8Array, timeout = 300): number {
  const r = new Uint8Array(24);
  const dv = new DataView(r.buffer);
  dv.setUint8(0, reqType); dv.setUint8(1, req);
  dv.setUint16(2, val & 0xffff, true); dv.setUint16(4, idx & 0xffff, true);
  dv.setUint16(6, data.length, true); dv.setUint32(8, timeout, true);
  dv.setBigUint64(16, ptr(data), true);
  const rc = libc.symbols.ioctl(fd, IOCTL.CONTROL, r);
  return rc < 0 ? -errno() : rc;
}

// usbdevfs bulk: ep, len, timeout, data*  — returns bytes transferred or -errno
export function bulk(ep: number, buf: Uint8Array, timeout = 3000): number {
  const r = new Uint8Array(24);
  const dv = new DataView(r.buffer);
  dv.setUint32(0, ep, true); dv.setUint32(4, buf.length, true); dv.setUint32(8, timeout, true);
  dv.setBigUint64(16, ptr(buf), true);
  const rc = libc.symbols.ioctl(fd, IOCTL.BULK, r);
  return rc < 0 ? -errno() : rc;
}

// microsecond busy-wait (sync). USB round-trips (~100µs) dominate, so sub-µs precision is irrelevant; the
// larger settles (rx_dck 600µs, lok 10ms) are honoured.
export function usleep(us: number): void {
  if (us <= 0) return;
  const end = performance.now() + us / 1000;
  while (performance.now() < end) { /* spin */ }
}

// --- register access (control 0xC0 read / 0x40 write, bRequest 0x05) ---
const rbuf = new Uint8Array(4);
export function r32(addr: number): number {
  // Zero the buffer EVERY read, like the C (uint8_t b[4]={0}): a failed/short control-IN must return 0, never
  // the previous read's bytes. A stale value here lets a poll false-pass and the next op run on a not-ready
  // chip, which wedges the DLE (BB reads then come back 0xeaeaeaea).
  rbuf[0] = 0; rbuf[1] = 0; rbuf[2] = 0; rbuf[3] = 0;
  control(0xC0, 0x05, addr & 0xffff, (addr >>> 16) & 0xff, rbuf, 300);
  return (rbuf[0] | (rbuf[1] << 8) | (rbuf[2] << 16) | (rbuf[3] << 24)) >>> 0;
}
export function w32(addr: number, val: number): void {
  val >>>= 0;
  const b = new Uint8Array([val & 0xff, (val >>> 8) & 0xff, (val >>> 16) & 0xff, (val >>> 24) & 0xff]);
  control(0x40, 0x05, addr & 0xffff, (addr >>> 16) & 0xff, b, 300);
}

// BB/PHY page is at USB +0x10000 (kernel BB 0x8000 == USB 0x18000). MAC regs stay direct.
export const BB = (a: number) => (0x10000 + a) >>> 0;
export function maskshift(m: number): number { let s = 0; m >>>= 0; if (!m) return 0; while (!((m >>> s) & 1)) s++; return s; }
export function w32m(a: number, mask: number, val: number): void {
  const o = r32(a), s = maskshift(mask);
  w32(a, ((o & ~mask) | ((val << s) & mask)) >>> 0);
}
export function bbset(bba: number, bits: number): void { const a = BB(bba); w32(a, (r32(a) | bits) >>> 0); }
export function bbclr(bba: number, bits: number): void { const a = BB(bba); w32(a, (r32(a) & ~bits) >>> 0); }
export function bbm(bba: number, mask: number, val: number): void { w32m(BB(bba), mask >>> 0, val >>> 0); }
export function bbr(bba: number, mask: number): number { const v = r32(BB(bba)); return ((v & mask) >>> maskshift(mask)) >>> 0; }

// RF direct (8852A): BB addr = rf_base[path] + (rf_addr<<2), 20-bit register. base A=0xc000 B=0xd000.
const RF_BASE = [0xc000, 0xd000];
export const RFREG_MASK = 0xfffff;
export function wrf(path: number, rfa: number, mask: number, val: number): void {
  const a = BB(RF_BASE[path] + ((rfa & 0xff) << 2));
  w32m(a, mask & RFREG_MASK, val); usleep(1);
}
export function rrf(path: number, rfa: number, mask: number): number {
  const v = r32(BB(RF_BASE[path] + ((rfa & 0xff) << 2))) & RFREG_MASK;
  return ((v & mask) >>> maskshift(mask)) >>> 0;
}
