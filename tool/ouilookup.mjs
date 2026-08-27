// ouilookup.mjs — resolve a MAC's OUI to a vendor, from nmap's local prefix table. Randomized (locally-
// administered) MACs have no real vendor from the address itself — but a probe request's vendor-specific IEs
// still carry the real maker's OUI, so vendorFromIEs recovers "Apple"/"Samsung"/… even behind a random MAC.
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
const lookup = (hex6) => (OUI || loadOui()).get(hex6.toUpperCase()) || null;
export const isRandom = (m) => (parseInt(m.slice(0, 2), 16) & 0x02) !== 0;
export const vendor = (mac) => (!mac || isRandom(mac)) ? null : lookup(mac.replace(/:/g, "").slice(0, 6));

// tidy a vendor string (drop corporate boilerplate) WITHOUT truncating the meaningful name.
export function vendorName(mac) {
  const v = vendor(mac); if (!v) return null;
  return v.replace(/,?\s*(Inc|Ltd|LLC|Corp|Co|GmbH|Technologies?|Technology|Electronics?|Computer|Systems?|Networks?|Communications?|Trading|International|Mobile)\b\.?/gi, "").replace(/\s{2,}/g, " ").trim() || v;
}

// vendor-specific IE OUIs that are generic (present on many devices) — ignore for device identification.
const GENERIC = new Set(["0050F2", "506F9A", "000FAC", "00904C", "001018", "0010F2", "0017F2AIR"]);
export function vendorFromIEs(ouis) {
  for (const o of ouis || []) {
    const hex = o.replace(/:/g, "").toUpperCase();
    if (GENERIC.has(hex)) continue;
    const v = lookup(hex);
    if (v) return v.replace(/,?\s*(Inc|Ltd|LLC|Corp|Co|GmbH|Technologies?|Technology|Electronics?|Computer|Systems?|Networks?|Communications?)\b\.?/gi, "").replace(/\s{2,}/g, " ").trim() || v;
  }
  return null;
}
