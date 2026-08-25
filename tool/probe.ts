// probe.ts — read the 18-byte USB device descriptor from a termux-usb fd, print vid:pid.
const fd = Number(Deno.args[0]);
const libc = Deno.dlopen("libc.so.6", { ioctl: { parameters: ["i32","u64","buffer"], result: "i32" } });
const ptr = (b: Uint8Array) => BigInt(Deno.UnsafePointer.value(Deno.UnsafePointer.of(b)));
const data = new Uint8Array(18);
const r = new Uint8Array(24); const dv = new DataView(r.buffer);
// GET_DESCRIPTOR device: bmRequestType 0x80, bRequest 0x06, wValue 0x0100, wIndex 0, len 18
dv.setUint8(0,0x80); dv.setUint8(1,0x06); dv.setUint16(2,0x0100,true); dv.setUint16(4,0,true);
dv.setUint16(6,18,true); dv.setUint32(8,1000,true); dv.setBigUint64(16,ptr(data),true);
const rc = libc.symbols.ioctl(fd, 0xC0185500n, r);
if (rc < 0) { console.log("ioctl fail", rc); Deno.exit(1); }
const vid = data[8] | (data[9]<<8), pid = data[10] | (data[11]<<8);
console.log(`vid:pid = ${vid.toString(16).padStart(4,"0")}:${pid.toString(16).padStart(4,"0")}`);
