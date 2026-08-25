const fd = Number(Deno.args[0]);
const libc = Deno.dlopen("libc.so.6", { ioctl: { parameters: ["i32","u64","buffer"], result: "i32" } });
const ptr = (b: Uint8Array) => BigInt(Deno.UnsafePointer.value(Deno.UnsafePointer.of(b)));
function r32(addr: number): number {
  const data = new Uint8Array(4);
  const r = new Uint8Array(24); const dv = new DataView(r.buffer);
  dv.setUint8(0,0xC0); dv.setUint8(1,0x05); dv.setUint16(2,addr&0xffff,true); dv.setUint16(4,(addr>>>16)&0xff,true);
  dv.setUint16(6,4,true); dv.setUint32(8,300,true); dv.setBigUint64(16,ptr(data),true);
  libc.symbols.ioctl(fd, 0xC0185500n, r);
  return (data[0]|(data[1]<<8)|(data[2]<<16)|(data[3]<<24))>>>0;
}
for (const a of [0x1e0, 0xF0, 0x88]) console.log(`0x${a.toString(16)}=0x${r32(a).toString(16)}`);
