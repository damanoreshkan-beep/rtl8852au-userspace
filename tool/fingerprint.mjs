// fingerprint.mjs — a rough device/OS guess from a probe-request's IE profile, for stations that hide behind a
// randomized MAC and a stripped probe. Not exact (modern phones deliberately minimize this) — always shown with
// a "?" — but the radio GENERATION (ax/ac/n) is solid, and P2P/Interworking/clean-Apple-probe are useful hints.
// fp = { tags:Set<ieId>, he:bool, vend:string[] (vendor-IE OUIs "aa:bb:cc") }.
export function deviceClass(fp) {
  if (!fp || !fp.tags || fp.tags.size === 0) return null;
  const has = (t) => fp.tags.has(t);
  const gen = fp.he ? "ax" : has(191) ? "ac" : has(45) ? "n" : "legacy";   // HE / VHT / HT / legacy-rates
  const vend = (fp.vend || []).map((o) => o.toLowerCase());
  const maker = vend.filter((o) => o !== "00:50:f2" && o !== "50:6f:9a" && o !== "00:0f:ac" && o !== "00:90:4c");
  let os = null;
  if (vend.includes("50:6f:9a")) os = "Android";                     // Wi-Fi Direct / P2P IE — Android-specific
  else if (fp.he && maker.length === 0 && has(127)) os = "Apple";    // clean modern probe (HE + ext-cap, no maker IE) — iOS/macOS style
  else if (has(107)) os = "Android";                                 // Interworking / Passpoint — leans Android
  return os ? `${os}?·${gen}` : gen;
}
