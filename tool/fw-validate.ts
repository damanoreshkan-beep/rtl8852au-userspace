// Decode rtw89 mfw container, then validate fwhdr_parse on each inner blob.
const raw = await Deno.readFile(Deno.args[0]);
const dv = new DataView(raw.buffer);
const le32 = (o: number) => dv.getUint32(o, true);

const sig = raw[0], fwNr = raw[1];
console.log(`mfw: sig=0x${sig.toString(16)} fw_nr=${fwNr}`);

const TYPE: Record<number, string> = { 0: "nic", 1: "nic_ce", 2: "ap", 3: "wowlan", 5: "mp" };
for (let i = 0; i < fwNr; i++) {
  const e = 16 + i * 16;
  const cv = raw[e], type = raw[e + 1], mp = raw[e + 2];
  const shift = le32(e + 4), size = le32(e + 8);
  console.log(`\nentry${i}: cv=U${cv} type=${type}(${TYPE[type] ?? "?"}) mp=${mp} off=0x${shift.toString(16)} size=${size}`);

  // parse inner fwhdr at `shift`
  const off = shift;
  const secNum = (le32(off + 6 * 4) >> 8) & 0xff;
  const hdrLen = 32 + secNum * 16;
  let body = off + hdrLen;
  const secs: string[] = [];
  for (let s = 0; s < secNum && s < 32; s++) {
    const d0 = le32(off + 32 + s * 16), d1 = le32(off + 32 + s * 16 + 4);
    let sz = d1 & 0xffffff;
    if (d1 & (1 << 28)) sz += 8;
    secs.push(`  sec${s}: dl=0x${(d0 & 0x1fffffff).toString(16)} len=${sz}${d1 & (1 << 29) ? " [redl]" : ""}`);
    body += sz;
  }
  const consumed = body - off;
  console.log(`  section_num=${secNum} hdr_len=${hdrLen} consumed=${consumed} vs size=${size} ${consumed === size ? "✓ EXACT — parser validated" : "✗ mismatch"}`);
  console.log(secs.join("\n"));
}
