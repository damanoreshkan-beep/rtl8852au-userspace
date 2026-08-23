# RESEARCH — RTL8852AU (ASUS USB-AX56) userspace monitor mode, no root

Reference driver: `lwfinger/rtl8852au` @ `dwa-x1850` (kernel module; we PORT, not build).
Transport already proven on-device: libusb `wrap_sys_device(fd)`, modeswitch `1a2b→1997`,
register control-transfer R/W. This note captures the firmware-download recipe so `initDriver`
is one pass.

Each claim tagged: **[V]** verified in source · **[I]** inferred · **[?]** open, must confirm in code.

## 1. Firmware images — pick by cut, station variant

`hal8852a_fw.h` declares many `array_8852a_<cut>_<feature>[len]` blobs. **[V]**
- cut: `u2` / `u3` / `u4` (wafer cut). feature: `nic` (station — what we want) / `ap` / `_mp` (production test — NOT us).
- For monitor/station use: **`array_8852a_u2_nic` = 360304 B** (or u3/u4 nic, by cut). **[V]**
- The blob is a C array in `hal8852a_fw.c` (36 MB total = all variants). Extract ONE nic array of the
  matching cut to a `.bin`, ship it in the APK. **[I]**

**Cut detection — RESOLVED (corrects an earlier note).** `R_AX_SYS_CFG1` is **0x00F0** (mac_reg.h), and
`B_AX_CHIP_VER` = bits[15:12]. So `cut = (reg32(0x00F0) >> 12) & 0xf`. Our on-device read `0x00F0 =
0x0c492537` therefore gives **cut = 2 = C-cut** — the earlier "wrong page / use 0x1000" remark was mistaken;
0x00F0 WAS the right register. fwdl rejects a mismatched cut with `FWDL_CUT_NOT_MATCH`, so select the inner
nic blob whose cv matches (this AX56 = cut 2). Implemented in `rtw_cpu.c::rtw_read_cut`, host-tested.

## 2. FW-download state machine — `mac_fwdl()` (fwdl.c:706)

Precondition: firmware CPU powered on (`mac_enable_fw`, fwdl.c:947) → state `MAC_AX_FWDL_CPU_ON`. **[V]**
Progress is polled through **`R_AX_WCPU_FW_CTRL` (0x1E0)**, read via our proven control-IN path. **[V]**

- **phase0** (fwdl.c:449): poll `0x1E0` bit `B_AX_H2C_PATH_RDY` (bit1) == 1. **[V]**
- **phase1** (fwdl.c:475): `__fwhdr_download()` (send fw header as a TX packet) → poll `0x1E0` bit
  `B_AX_FWDL_PATH_RDY` (bit2) == 1 → write 0 to `R_AX_HALT_H2C_CTRL` and `R_AX_HALT_C2H_CTRL`. **[V]**
- **phase2** (fwdl.c:549): `__sections_download()` per section → `PLTFM_DELAY_MS(5)` → `check_fw_rdy()`
  polls `0x1E0[7:5] == FWDL_WCPU_FW_INIT_RDY (7)`. Errors: `FWDL_CHECKSUM_FAIL`, `FWDL_SECURITY_FAIL`,
  `FWDL_CUT_NOT_MATCH`. **[V]**

## 3. How bytes reach the chip — this is the crux for libusb

FW header and sections are sent as **H2C TX packets**, NOT register-FIFO writes: **[V]**
- `__sections_download` (fwdl.c:293): split section into chunks of `FWDL_SECTION_PER_PKT_LEN`, each into an
  `h2cb`, prepend a **TX descriptor** (`__sections_build_txd` → `build_txdesc` / `txdesc_len`), send via
  **`PLTFM_TX(h2cb)`**. **[V]**
- On USB, `PLTFM_TX` == **bulk OUT transfer** (our EP 0x05/0x06…). So: `[txdesc][fw chunk]` → bulk OUT,
  loop, poll 0x1E0 between phases. **[I]**

So userspace needs exactly two primitives, both already within reach:
1. **register R/W** — control 0xC0/0x05 (read, PROVEN) + 0x40/0x05 (write) — polling + power-on. **[V/I]**
2. **TX packet** — bulk OUT with a txdesc header — fw dl AND later frame TX. **[I]**

## 3a. FW blob layout + packet shapes (RESOLVED)

`fwhdr_parser` (fwdl.c:84) + `fwhdr_hdr_parser` (:65). Blob layout: **[V]**
```
[ fwhdr_hdr_t          ]  FWHDR_HDR_LEN = sizeof(fwhdr_hdr_t)            [? struct size]
[ fwhdr_section_t × N  ]  FWHDR_SECTION_LEN = sizeof(fwhdr_section_t)    [? struct size]
[ section0 body ][ section1 body ]...   (back-to-back; each desc gives its len)
```
- `section_num = (hdr_word >> 8) & 0xff` (FWHDR_SEC_NUM_SH=8, MSK=0xff). **[V]**
- `hdr_len = FWHDR_HDR_LEN + section_num * FWHDR_SECTION_LEN`. **[V]**
- Parser enforces Σ(section lens) + hdr_len == total len, else MACFWBIN — free integrity check. **[V]**

Two packet shapes onto bulk OUT: **[V]**
- **Header packet** (`__fwhdr_download` :154): first `hdr_len` bytes → H2C cmd
  `h2c_pkt_set_hdr_fwdl(FWCMD_H2C_CL_FWDL, FWCMD_H2C_FUNC_FWHDR_DL)` → `h2c_pkt_build_txd` → PLTFM_TX.
  Shape `[txdesc][H2C-cmd-hdr][fw header + section descs]`.
- **Section packets** (`__sections_download` :293): chunk each section body at
  **FWDL_SECTION_PER_PKT_LEN = 2020 B**, `__sections_push` reserves 8 B, txdesc type
  `RTW_PHL_PKT_TYPE_FWDL`. Shape `[txdesc(FWDL)][8B push][≤2020 B chunk]`.
- Poll spin const `FWDL_WAIT_CNT = 400000`. **[V]**

**Leaf layouts — RESOLVED (2 of 3) + one still open:**
1. ✅ `fwhdr_hdr_t` = 8×u32 (32 B), `fwhdr_section_t` = 4×u32 (16 B). Fields (fwdl.h/fwcmd_intf.h):
   - header: `section_num=(le32(dword6)>>8)&0xff`; before send patch `dword7` low16 (`FW_PART_SZ`) = 2020.
   - section: `dladdr=le32(dword0)&0x1FFFFFFF`; `size=le32(dword1)&0xFFFFFF (+8 if BIT28 checksum)`;
     `redl=BIT29`.
2. ✅ H2C `fwcmd_hdr` = 8 B: `hdr0 = cat(0x1) | class(0x3)<<2 | func(0x0)<<8 | type(0)<<16 | seq<<24`
   (fwdl header ⇒ `hdr0=0x0D`); `hdr1 = data_len & 0x3FFF`. (rack=dack=0 for fwdl.)
3. ✅ USB txdesc = `wd_body_t` = **24 B** (6×u32; no WD_INFO for H2C/FWDL). From `trx_desc.c`
   `txdes_proc_h2c_fwdl`: `dword0 = (MAC_AX_DMA_H2C=12)<<16 | (section? AX_TXD_FWDL_EN=BIT20 : 0)`;
   `dword2 = pktlen & 0x3FFF`; rest 0. Header packet uses is_fwdl=0 (H2C cmd), sections is_fwdl=1.
   Section packets carry an 8-B `__sections_push` prefix (contents want a capture to confirm; zeroed).

**ENCODER VERIFIED (host):** `native/test/test_fwdl.c` compiles `rtw_fwdl.c` and runs it over the real
U2-nic blob with a fake ready-chip. All phases complete; **194 packets, byte-exact**: header txdesc
`d0=0x000C0000` pktsize=88, `fwcmd hdr0=0x0D`; every section packet has FWDL_EN + DMA ch=H2C(12); chunking
at 2020 (178+14+1 = 193 section pkts). The whole fw-download encoder is proven; only the on-chip run
(after power-on) is unproven.

### 3b. Parser VALIDATED on real firmware (not just transcribed)
Ran `fwhdr_parse` over kernel.org `linux-firmware/rtw89/rtw8852a_fw.bin`. That file is an rtw89 **mfw
container** (`sig=0xFF`, `fw_nr=4`, 16-byte header then 16-byte entries `{cv,type,mp,rsvd,le32 shift,
le32 size}`; the vendor `hal8852a_fw.c` array is the raw INNER blob with no wrapper). All 4 inner blobs
parsed with EXACT byte-consumption: **[V]**
```
cv=U1 nic    section_num=3 hdr_len=80  Σ+hdr = 387136 == size ✓
cv=U2 nic    section_num=3 hdr_len=80  Σ+hdr = 386304 == size ✓
cv=U1 wowlan / cv=U2 wowlan                       ✓ / ✓
sec dl addrs: 0x18970000, 0x18e10000, 0x18e17c00[redl]  (valid RTL8852A SRAM, ROM_ADDR 0x18900000)
```
So: pick the inner blob by **cv (cut) + type=nic** for monitor. Cut comes from the MAC-page cut register
(reads real data, confirmed on-device). The parser and header/section encoding are proven correct.

## 4. Monitor mode + RX (after fw ready)

- Reference: `core/monitor/rtw_radiotap.c` — how the driver flags monitor and builds the radiotap header. **[V]**
- RX path: bulk IN (EP 0x84) delivers `[rxdesc][802.11 frame]`. RSSI + channel + rate live in the
  **rxdesc PHY-status**, not in the 802.11 frame — strip rxdesc, map fields, feed frame-parser. **[I]**
- **rxdesc layout — RESOLVED + host-verified** (rxdesc.h / trx_desc.c rxdes_parse_comm). rxd_short=16B,
  rxd_long=32B. dword0: pktsize=d0&0x3fff, shift=(d0>>14)&3, rpkt_type=(d0>>24)&0xf (WIFI=0),
  drvsize=(d0>>28)&7. **802.11 frame at `rxdlen + drvsize*8 + shift`**; crc_err=dword3&BIT9; rate=(dword2>>16)&0x1ff.
  Ported to `rtw_rx.c::rtw_rxd_parse`, host-tested (`native/test/test_rxd.c`). frame-parser strips it first.
- **Monitor enable — register found** (mac_reg.h): `R_AX_RX_FLTR_OPT` = **0xCE20**, `B_AX_SNIFFER_MODE` = BIT(0)
  (+ A_A1_MATCH/A_BC/A_MC accept bits). `rtw_rx.c::rtw_monitor_enable` sets them. WRITTEN, not HW-verified.
- **Still open: RSSI** lives in a PPDU-status rpkt (separate type), not the wifi rxd — TODO when RX runs.
- **THE big remaining block: MAC/BB/RF init + RF calibration** — thousands of register-table entries plus
  calibration routines. This is most of the driver and realistically needs on-hardware iteration; it is NOT
  something to port blind. Without it the MAC delivers no frames, so monitor/scan cannot be exercised yet.
  Channel set is an H2C command (not a raw reg write) and belongs to this block.

## 5. Power-on before fwdl — RESOLVED + host-verified

Power-on is a **data-driven table walk** (`pwr.c` engine + `pwr_seq_8852a.c` tables), using only byte
register R/W + delay — the transport we already drive. `struct mac_pwr_cfg` {u16 addr; u8 cut_msk; u8
intf_msk; u8 base:4,cmd:4; u8 msk; u8 val}. Engine (`sub_pwr_seq_start`): for each entry passing
`intf_msk & intf` and `cut_msk & cut` — **WRITE** = R8/mask/W8, **POLL** = spin on R8&msk==val&msk (2000×,
1ms), **DELAY**, until **END**. `base` unused (addr taken raw). **[V]**

Cut masks: CAV=BIT0, CBV=BIT1, CCV=BIT2, … CVALL=0xFF. Intf: USB2=BIT1, USB3=BIT2 (AX56 = USB2). Ported the
NIC table `mac_pwron_nic_8852a` (25 entries) 1:1 into `native/cpp/rtw_pwron.c`. **Host-verified**
(`native/test/test_pwron.c`): for USB2/CBV exactly **16 register writes + 2 polls** execute, PCIE/SDIO/CAV-only
entries correctly filtered out, both polls satisfied, engine returns 0. All ops land on the low MAC page
(0x0005/0x0006/0x0088/0x0080/0x0024/0x02A0/0x02A2/0x106D) — addresses we already read on-device. **[V]**

Remaining before an on-chip run: map the cut register → CAV/CBV/CCV (read confirmed, field decode TODO);
`get_usb_mode` (USB2 vs USB3); the `mac_pwr_switch` boot-mode preamble (RMW on SYS_PW_CTRL/SYS_STATUS1/
GPIO_MUXCFG/RSV_CTRL — noted in rtw_pwron.c); then `mac_enable_fw` (CPU_ON).
Order: **power-on → enable_fw(CPU_ON) → mac_fwdl → MAC/BB/RF init → monitor enable → channel hop → RX**.

## 6. dmac_pre_init + dle_init(DLFW) — the block that lifts H2C_PATH_RDY (fully mapped)

Hardware pinpointed this as the missing step before fwdl (see docs/STATUS.md on-chip findings). Full recipe:

**dmac_pre_init (8852A), before enable_cpu:**
- `W32(R_AX_DMAC_FUNC_EN=0x8400, MAC_FUNC_EN(BIT30)|DMAC_FUNC_EN(BIT29)|DISPATCHER_EN(BIT18)|PKT_BUF_EN(BIT22))`
- `W32(R_AX_DMAC_CLK_EN=0x8404, DISPATCHER_CLK_EN(BIT18))`
- then `dle_init(MAC_AX_QTA_DLFW)`
- ROOT CAUSE the first on-chip try failed: HCI_FUNC_EN(0x8380) was written BEFORE 0x8400 — it is in the DMAC
  clock domain and will not hold until DMAC_FUNC_EN is set.

**dle_init(DLFW) — USB 8852A config** (dle.c `dle_mem_usb_8852a` DLFW = wde_size4/ple_size4/wde_qt4/ple_qt13):
- wde_size4: page=64B, lnk=0, unlnk=4096.  ple_size4: page=128B, lnk=64, unlnk=1472.
- wde_qt4 (min=max) all 0 (wcpu patched from ext_mode).  ple_qt13 (min=max): c2h=16, h2c=48, rest 0.
- sequence: `dle_func_en(DIS)` [DMAC_FUNC_EN clear DLE_WDE_EN|DLE_PLE_EN] → `dle_clk_en(EN)` [DMAC_CLK_EN set
  DLE_WDE_CLK_EN|DLE_PLE_CLK_EN] → `dle_mix_cfg` → `wde_quota_cfg`+`ple_quota_cfg` → `dle_func_en(EN)` → poll done.
- dle_mix_cfg: R_AX_WDE_PKTBUF_CFG (page_sel + start_bound=0 + free_page_num=lnk); R_AX_PLE_PKTBUF_CFG
  (page_sel + start_bound=(wde lnk+unlnk)*pgsz/DLE_BOUND_UNIT + free_page_num=lnk).
- wde_quota_cfg: R_AX_WDE_QTA0/1/3/4_CFG (min|max hif/wcpu/pkt_in/cpu_io). ple_quota_cfg: R_AX_PLE_QTA*.
- sanity dle_init enforces: dle_used_size(wde,ple) == fifo_size - dle_rsvd_size — host-testable.

**Then**: hci_func_en (R_AX_HCI_FUNC_EN=0x8380 |= HCI_TXDMA_EN(BIT0)|HCI_RXDMA_EN(BIT1)) → enable_cpu(dlfw=1)
→ fwdl. Remaining work = pull the ~15 WDE/PLE register addresses + write rtw_dle.c (host-test the used_size
sanity, then on-chip). This is the LAST block before firmware downloads on-chip.

## Bottom line
Nothing here needs root or a kernel. Power-on and fw-download are both ported and **host-verified byte-exact**,
and power-on + enable_cpu are **validated on real silicon**. The one remaining block before fwdl runs on-chip
is dmac_pre_init + dle_init(DLFW) (§6, fully mapped). After that: MAC/BB/RF init + monitor RX (§4). Every
remaining step has a file address.
