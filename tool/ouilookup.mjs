// ouilookup.mjs — resolve a MAC's OUI (first 3 octets) to a vendor, from nmap's local prefix table.
// Randomized (locally-administered) MACs have no real vendor and resolve to null.
let OUI = null;
const DB = "/usr/share/nmap/nmap-mac-prefixes";
export function loadOui(path = DB) {
  if (OUI) return OUI;
  OUI = new Map();
  try {
    for (const line of Deno.readTextFileSync(path).split("\n")) {
      if (line.length < 8 || line[0] === "#" || line[6] !== " ") continue;   // "AABBCC Vendor…"
      OUI.set(line.slice(0, 6).toUpperCase(), line.slice(7).trim());
    }
  } catch { /* no DB -> resolution disabled, MACs shown bare */ }
  return OUI;
}
export const isRandom = (m) => (parseInt(m.slice(0, 2), 16) & 0x02) !== 0;
export function vendor(mac) {
  if (!mac || isRandom(mac)) return null;
  return (OUI || loadOui()).get(mac.replace(/:/g, "").slice(0, 6).toUpperCase()) || null;
}
// a short label for tight columns: first meaningful word, trimmed.
export function vendorShort(mac, n = 12) {
  const v = vendor(mac); if (!v) return null;
  const w = v.replace(/,?\s*(Inc|Ltd|LLC|Corp|Co|GmbH|Technologies?|Technology|Electronics|Computer|Systems?|Networks?|Communications?)\b\.?/gi, "").trim() || v;
  return w.length > n ? w.slice(0, n - 1) + "…" : w;
}
