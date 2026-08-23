# AX56 control — feature / mode status (honest)

Legend: ✅ proven on hardware this session · 🟡 code written, untested on-air · ⛔ not yet (needs port) · 🚫 declined

| Mode / feature            | State | Where it lives | Blocker |
|---------------------------|-------|----------------|---------|
| Enumerate USB devices     | ✅ | tool `list`, native UsbManager | — |
| Identify (VID/PID/desc)   | ✅ | tool `id`, `usbfull.ts` | — |
| Modeswitch storage→wifi   | ✅ | tool `switch`, `modeswitch.cpp` | — |
| Register read (rtw_read32)| ✅ | tool `reg` (low + MAC page) | — |
| Register write            | 🟡 | tool `regw` | not yet exercised on a safe reg |
| Cut detection (fw select) | ✅ | `rtw_read_cut` | R_AX_SYS_CFG1=0x00F0, cut=(v>>12)&0xf; measured 2 (C-cut) on-device |
| CPU enable/disable (dlfw)  | ✅ | `rtw_cpu.c` + `native/test` | host-verified (CPU_CLK/WCPU_EN/FWDL_EN/boot_reason) |
| Init chain integration     | 🟡 | `native-bridge.cpp` | written: cut→pwron→cpu→fwdl over libusb; NDK-only, first on-chip run pending |
| Power-on sequence         | ✅ | `rtw_pwron.c` + `native/test` | engine + NIC table ported 1:1, host-verified (16 USB2 ops, filtering correct) |
| FW header/section parse   | ✅ | `rtw_fwdl.c::fwhdr_parse` | validated byte-exact on real fw (RESEARCH §3b) |
| FW download encoder       | ✅ | `rtw_fwdl.c` + `native/test` | compiled + byte-verified on real fw: 194 pkts, txdesc/fwcmd/chunking all correct |
| dmac_pre_init + dle(DLFW)  | ✅ | `rtw_dle.c` + `native/test` | ported; host-verified: used_size==fifo(458752), bound=32, h2c=48, HCI-DMA holds |
| FW download ON HARDWARE    | ⛔ | ready to re-test on chip | full chain wired (pwron→dmac/dle→hci→cpu→fwdl); needs wifi-mode + clean chip |
| MAC/BB/RF init + calib     | ⛔ | — | THE big remaining block; huge reg tables + RF cal, needs on-HW iteration |
| RX descriptor strip        | ✅ | `rtw_rx.c` + `native/test` | host-verified (offset=rxdlen+drv*8+shift, rate/crc) |
| Monitor(sniffer) enable    | 🟡 | `rtw_rx.c::rtw_monitor_enable` | R_AX_RX_FLTR_OPT 0xCE20 SNIFFER_MODE; written, not HW-verified |
| Channel hopping 2.4/5 GHz  | 🟡 | `channel-hop.cpp` (skeleton) | channel set = H2C, not raw reg |
| RX raw 802.11 frames       | 🟡 | `frame-parser.cpp` strips rxd | parser+strip done; no frames until MAC/BB/RF init |
| Scan (SSID/BSSID/ch/rate)  | 🟡 | parser extracts SSID/BSSID/ch | RSSI needs PPDU-status; no RX feed until init |
| Frame injection (generic) | 🟡 | `injectRaw()` bulk-OUT primitive | txdesc + fw ready |
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
