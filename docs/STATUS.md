# AX56 control — feature / mode status (honest)

Legend: ✅ proven on hardware this session · 🟡 code written, untested on-air · ⛔ not yet (needs port) · 🚫 declined

*** ✅ 2026-08-25 — RX **and** TX BOTH PROVEN NO-ROOT, ON THE BOX AND ON AN ANDROID PHONE. ***
The proven path is `tool/hwdriver.c` (C + libusb), run on the box directly or on Android via `termux-usb`
(the passed usbfs fd is wrapped with `libusb_wrap_sys_device` + `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`). One
cold bring-up does it:
- **fwdl** — `hwburst_fwdl` (HB), with a warm-stop CPU reset so it boots a cold *or* warm chip.
- **RX / monitor** — cold chip + replay the monitor-on-ch6 tail of a captured kernel bring-up
  (`full_ch6.bin` @ start ≈ 2329470), no live cal. EP `0x84` delivers real 802.11: this session, from the
  phone, `t0=138 WIFI / t1=138 PPDU / t10=24 CSI`, 94 beacons — including a **controlled** transmitter
  (box `iwlwifi` + scapy beacon, SSID `AX56RXTEST` / BSSID `02:ca:fe:12:34:56`, ch6) caught **90×**
  in the radiotap pcap.
- **TX / injection** — same bring-up + live `RCK/DACK/IQK/TSSI/DPK` cal, then `[txdesc][frame]` on EP `0x05`.
  From the phone: 900 frames injected, 360 caught by a second receiver.

The capability table and the journey below are kept as the honest record; the old "not achievable" verdict at
the very bottom is **superseded** by this result.

| Mode / feature            | State | Where it lives | Blocker |
|---------------------------|-------|----------------|---------|
| Enumerate USB devices     | ✅ | tool `list`, native UsbManager | — |
| Identify (VID/PID/desc)   | ✅ | tool `id`, `usbfull.ts` | — |
| Modeswitch storage→wifi   | ✅ | tool `switch`, `modeswitch.cpp` | — |
| Register read (rtw_read32)| ✅ | tool `reg` (low + MAC page) | — |
| Register write            | ✅ | tool `regw` | exercised on-HW (e.g. 0x88 WCPU_EN clear for wedge-recovery) |
| Cut detection (fw select) | ✅ | `rtw_read_cut` | R_AX_SYS_CFG1=0x00F0, cut=(v>>12)&0xf; measured 2 (C-cut) on-device |
| CPU enable/disable (dlfw)  | ✅ | `rtw_cpu.c` + `native/test` | host-verified (CPU_CLK/WCPU_EN/FWDL_EN/boot_reason) |
| Init chain integration     | 🟡 | `native-bridge.cpp` | written: cut→pwron→cpu→fwdl over libusb; NDK-only, first on-chip run pending |
| Power-on sequence         | ✅ | `rtw_pwron.c` + `native/test` | engine + NIC table ported 1:1, host-verified (16 USB2 ops, filtering correct) |
| FW header/section parse   | ✅ | `rtw_fwdl.c::fwhdr_parse` | validated byte-exact on real fw (RESEARCH §3b) |
| FW download encoder       | ✅ | `rtw_fwdl.c` + `native/test` | compiled + byte-verified on real fw: 194 pkts, txdesc/fwcmd/chunking all correct |
| dmac_pre_init + dle(DLFW)  | ✅ | `rtw_dle.c` + `native/test` | ported; host-verified: used_size==fifo(458752), bound=32, h2c=48, HCI-DMA holds |
| FW download ON HARDWARE    | ✅ | `hwdriver.c::hwburst_fwdl` | STS=7 BOOTED on box + phone; warm-stop boots cold *or* warm chip |
| MAC/BB/RF init + calib     | ✅ | `hwdriver.c` replay tail + live cal | cracked via one-clean-bring-up replay (`full_ch6.bin`) + live RCK/DACK/IQK/TSSI/DPK |
| RX descriptor strip        | ✅ | `rtw_rx.c` + `native/test` | host-verified (offset=rxdlen+drv*8+shift, rate/crc) |
| Monitor(sniffer) enable    | ✅ | `hwdriver.c` (RX_FLTR 0xCE20) | HW-verified on box + phone; final filter 0x03174438 |
| Channel hopping 2.4/5 GHz  | ✅ | per-channel bring-up blob | all 39 channels (14×2.4 + 25×5 GHz) captured + replayed; warm re-init, no replug |
| RX raw 802.11 frames       | ✅ | `hwdriver.c` EP 0x84 parse | real beacons + data + PPDU-RSSI, radiotap pcap (box + phone) |
| Scan (SSID/BSSID/ch/rate)  | 🟡 | EP 0x84 parser | RX feed live now; SSID/BSSID/RSSI parsed; a scan UI on top is TODO |
| Frame injection (generic) | ✅ | `hwdriver.c` EP 0x05 | on-air, verified by a 2nd receiver (box + phone) |
| Deauth attack             | 🚫 | — | declined: DoS, no authz context |

## The critical path to a working scanner
`power-on → enable_fw → mac_fwdl → MAC/BB/RF init → monitor enable → channel hop → bulk-IN RX → parse`

Everything left of "monitor enable" is firmware/init porting; everything right is transport we already
drive. The single highest-leverage next step is the **firmware download**, because nothing downstream can
be validated until the chip's CPU is running our firmware. Its recipe is fully mapped (RESEARCH §3a); it
needs exactly three byte-layouts from the reference driver, then phase0/1/2 becomes code.

## What is genuinely runnable today
`tool/ax56ctl.sh` — enumerate, identify, modeswitch, register read/write — all verified on the AX56 this
session over `termux-usb` + usbfs, no root. The native app (`native/`) is the UI vehicle for the same
primitives; its monitor/scan path is stubbed to the state above, not faked.

## On-hardware findings (first chip run, 2026-08-23)
Ran the ported chain on the live AX56 via the Deno/usbfs runner (same primitives as native-bridge):
- ✅ **power-on table validated on silicon** (all polls passed, cut=2 correct)
- ✅ enable_cpu partially worked (WFC=0x1, FWDL_EN set)
- ⛔ **fwdl blocked at phase0 H2C_PATH_RDY** — needs the full MAC **sys_init/dmac_init** (the big block)
   before the H2C path is live; `usb_pre_init` (HCI DMA @0x8380) alone is not enough.
- ⚠️ **fwdl without full init WEDGES the chip** (all reg reads → 0xffffffff); recover by physical replug.

Net: hardware confirms the remaining work is MAC sys_init/DMAC init, and that it cannot be skipped.

### Second chip run (dmac_pre_init + dle + usb_pre_init added)
- ✅ **DMAC + DLE(DLFW) init validated on silicon**: WDE_INI(0x8D00)=0x3, PLE_INI(0x9100)=0x3 (init-ready).
- ✅ **HCI_FUNC_EN(0x8380) now holds = 0x3** (was 0x0) — root cause fixed: DMAC must be enabled first.
- ✅ usb_pre_init: USB TX/RX RST released (0x1174), matching driver intf_pre_init order.
- ⛔ **H2C_PATH_RDY STILL does not set** after the full documented pre-init (power-on → dmac_pre_init →
  usb_pre_init/hci → disable/enable_cpu). This is the current hard wall.
- Confounders to rule out on a FRESH replug: PLATFORM_ENABLE read 0x54f on entry (WCPU_EN already set from
  prior partial runs — CPU may not be getting a clean reset); `disable_fw_watchdog` was omitted from
  disable_cpu; ROM boot may need a longer settle or a step not obvious from static reading. Needs a
  clean-chip baseline + deeper CPU-boot investigation before more attempts (2-attempt rule hit here).

### Third run — FRESH chip, confounders ruled out
Physically replugged (clean disk mode) → modeswitch → immediate init. **Identical result**: DMAC/DLE/HCI all
up (WDE/PLE_INI=0x3, HCI=0x3), WFC=0xc0 fresh, but **H2C_PATH_RDY still 0**. PLATFORM_ENABLE=0x54f on a fresh
chip too (WCPU_EN set by the ROM's own boot). So the "dirty chip" theory is WRONG — the wall is real and
reproducible from a clean state. STOPPING hardware iteration (2-attempt rule firmly hit).

**Honest assessment of the H2C_PATH_RDY wall:** power-on, DMAC, DLE(DLFW), USB/HCI init are all validated on
silicon — a large, real result. But the CPU→download handshake does not arm.

**Five distinct hypotheses tested on hardware, all fail identically:** (1) no DMAC → added DMAC; (2) DMAC+DLE
→ DLE init-ready but H2C 0; (3) + usb_pre_init (USB TX/RX RST released) → H2C 0; (4) fresh replugged chip →
identical; (5) **+ full power-OFF cold reset first** (PLATFORM_ENABLE 0x54f→0x54c: WCPU_EN cleared, confirmed
cold) → power-on/DMAC/DLE/HCI all back up, **H2C_PATH_RDY still 0**. So it is NOT: missing DMAC/DLE, USB reset,
a dirty chip, or a warm CPU. The wall is robust and reproducible.

**Verdict:** cracking the fwdl handshake is beyond static source-reading + hypothesis-testing on this bench.
The whole init up to fwdl is correct and validated; something in the CPU→download arming is either undocumented
in the vendor source path we can see, or needs the exact on-the-wire order (a USB capture of the working driver)
or hardware/secure-boot state we can't reach no-root. **No-root userspace monitor mode is not achievable via
this route in reasonable effort** — the kernel driver + root remains the only proven monitor path for 8852AU.
This repo stands as a validated bring-up up to the fwdl wall, honestly documented.

*** SUPERSEDED (2026-08-25). The verdict above was wrong. The fwdl handshake and the full MAC/BB/RF bring-up
were cracked (record-and-replay a single clean monitor bring-up against one hwburst fw boot, + live RF cal for
TX). See the ✅ banner at the top: RX and TX both work no-root — on the box **and** on an Android phone over
termux-usb. No-root userspace monitor mode IS achievable via this route. ***
