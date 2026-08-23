/*
 * rtw_dle.c — RTL8852AU DMAC pre-init + DLE (packet-buffer) init for firmware download.
 * Ported from lwfinger/rtl8852au @ dwa-x1850 init.c (dmac_pre_init) + dle.c (dle_init,
 * USB-8852A MAC_AX_QTA_DLFW config) + mac_reg.h addresses.
 *
 * This is the block that lifts H2C_PATH_RDY — hardware showed fwdl blocks without it.
 * Runs AFTER power-on, BEFORE enable_cpu:  rtw_pwron -> rtw_dmac_pre_init -> rtw_hci_func_en
 * -> rtw_enable_cpu -> rtw_fwdl.  ROOT CAUSE of the first on-chip failure: HCI_FUNC_EN (0x8380)
 * is in the DMAC clock domain and won't hold until DMAC_FUNC_EN (0x8400) is set — done here first.
 *
 * The DLFW quota config is host-verified for internal consistency (used_size == fifo_size,
 * native/test/test_dle.c). Register writes are not yet on-chip validated.
 */
#include <stdint.h>
#include "rtw_io.h"

#ifndef BIT
#define BIT(n) (1u << (n))
#endif
#define SET_WORD(v, sh, msk) (((uint32_t)(v) & (msk)) << (sh))

/* --- registers (mac_reg.h) --- */
#define R_AX_DMAC_FUNC_EN   0x8400
#define R_AX_DMAC_CLK_EN    0x8404
#define R_AX_HCI_FUNC_EN    0x8380
#define R_AX_WDE_PKTBUF_CFG 0x8C08
#define R_AX_PLE_PKTBUF_CFG 0x9008
#define R_AX_WDE_QTA0_CFG   0x8C40
#define R_AX_WDE_QTA1_CFG   0x8C44
#define R_AX_WDE_QTA3_CFG   0x8C4C
#define R_AX_WDE_QTA4_CFG   0x8C50
#define R_AX_PLE_QTA0_CFG   0x9040   /* Q_n = base + n*4 */
#define R_AX_WDE_INI_STATUS 0x8D00
#define R_AX_PLE_INI_STATUS 0x9100

/* --- bits --- */
#define B_MAC_FUNC_EN       BIT(30)
#define B_DMAC_FUNC_EN      BIT(29)
#define B_DLE_WDE_EN        BIT(26)
#define B_DLE_PLE_EN        BIT(23)
#define B_PKT_BUF_EN        BIT(22)
#define B_DISPATCHER_EN     BIT(18)
#define B_DLE_WDE_CLK_EN    BIT(26)
#define B_DLE_PLE_CLK_EN    BIT(23)
#define B_DISPATCHER_CLK_EN BIT(18)
#define B_HCI_TXDMA_EN      BIT(0)
#define B_HCI_RXDMA_EN      BIT(1)
#define WDE_MGN_INI_RDY     0x3       /* Q_MGN(BIT1)|BUF_MGN(BIT0) */
#define PLE_MGN_INI_RDY     0x3

/* pktbuf cfg fields */
#define PAGE_SEL_SH 0
#define PAGE_SEL_MSK 0x3
#define START_BOUND_SH 8
#define START_BOUND_MSK 0x3f
#define FREE_PAGE_NUM_SH 16
#define FREE_PAGE_NUM_MSK 0x1fff
/* ple quota field: min[11:0] | max[27:16] */
#define QTA_MIN_SH 0
#define QTA_MAX_SH 16
#define QTA_MSK 0xfff

/* page-sel enum values (dle.h) + byte sizes + bound unit */
#define SEL_WDE_64  0
#define SEL_PLE_128 1
#define DLE_BOUND_UNIT (8 * 1024)

/* --- USB-8852A MAC_AX_QTA_DLFW config (dle.c dle_mem_usb_8852a) --- */
#define WDE_PGSZ 64
#define WDE_LNK  0
#define WDE_UNLNK 4096
#define PLE_PGSZ 128
#define PLE_LNK  64
#define PLE_UNLNK 1472
#define DLFW_PLE_C2H 16     /* ple_qt13.c2h  -> PLE Q2 */
#define DLFW_PLE_H2C 48     /* ple_qt13.h2c  -> PLE Q3 */
#define FIFO_SIZE_8852A 458752

#define DLE_WAIT_CNT 2000

/* host-testable: dle.c dle_used_size (rsvd is 0 for DLFW) */
uint32_t rtw_dle_used_size(void) {
    return (uint32_t)(WDE_PGSZ * (WDE_LNK + WDE_UNLNK)) +
           (uint32_t)(PLE_PGSZ * (PLE_LNK + PLE_UNLNK));
}
uint32_t rtw_dle_fifo_size(void) { return FIFO_SIZE_8852A; }

/* dle_init(MAC_AX_QTA_DLFW) — the packet-buffer setup fwdl needs */
static int dle_init_dlfw(struct rtw_io *io) {
    uint32_t v;
    /* dle_func_en(DIS): DMAC_FUNC_EN clear WDE/PLE */
    v = io->reg_read32(io->ctx, R_AX_DMAC_FUNC_EN) & ~(B_DLE_WDE_EN | B_DLE_PLE_EN);
    io->reg_write32(io->ctx, R_AX_DMAC_FUNC_EN, v);
    /* dle_clk_en(EN): DMAC_CLK_EN set WDE/PLE clk */
    v = io->reg_read32(io->ctx, R_AX_DMAC_CLK_EN) | B_DLE_WDE_CLK_EN | B_DLE_PLE_CLK_EN;
    io->reg_write32(io->ctx, R_AX_DMAC_CLK_EN, v);

    /* dle_mix_cfg */
    v = io->reg_read32(io->ctx, R_AX_WDE_PKTBUF_CFG);
    v = (v & ~((uint32_t)PAGE_SEL_MSK << PAGE_SEL_SH))       | SET_WORD(SEL_WDE_64, PAGE_SEL_SH, PAGE_SEL_MSK);
    v = (v & ~((uint32_t)START_BOUND_MSK << START_BOUND_SH)) | SET_WORD(0, START_BOUND_SH, START_BOUND_MSK);
    v = (v & ~((uint32_t)FREE_PAGE_NUM_MSK << FREE_PAGE_NUM_SH)) | SET_WORD(WDE_LNK, FREE_PAGE_NUM_SH, FREE_PAGE_NUM_MSK);
    io->reg_write32(io->ctx, R_AX_WDE_PKTBUF_CFG, v);

    uint32_t bound = (uint32_t)(WDE_LNK + WDE_UNLNK) * WDE_PGSZ / DLE_BOUND_UNIT;   /* = 32 */
    v = io->reg_read32(io->ctx, R_AX_PLE_PKTBUF_CFG);
    v = (v & ~((uint32_t)PAGE_SEL_MSK << PAGE_SEL_SH))       | SET_WORD(SEL_PLE_128, PAGE_SEL_SH, PAGE_SEL_MSK);
    v = (v & ~((uint32_t)START_BOUND_MSK << START_BOUND_SH)) | SET_WORD(bound, START_BOUND_SH, START_BOUND_MSK);
    v = (v & ~((uint32_t)FREE_PAGE_NUM_MSK << FREE_PAGE_NUM_SH)) | SET_WORD(PLE_LNK, FREE_PAGE_NUM_SH, FREE_PAGE_NUM_MSK);
    io->reg_write32(io->ctx, R_AX_PLE_PKTBUF_CFG, v);

    /* wde_quota_cfg: all zero for DLFW (Q0/1/3/4) */
    io->reg_write32(io->ctx, R_AX_WDE_QTA0_CFG, 0);
    io->reg_write32(io->ctx, R_AX_WDE_QTA1_CFG, 0);
    io->reg_write32(io->ctx, R_AX_WDE_QTA3_CFG, 0);
    io->reg_write32(io->ctx, R_AX_WDE_QTA4_CFG, 0);

    /* ple_quota_cfg: Q0..Q10 zero except Q2=c2h, Q3=h2c (min=max for DLFW) */
    for (int q = 0; q <= 10; q++) {
        uint32_t val = 0;
        if (q == 2) val = SET_WORD(DLFW_PLE_C2H, QTA_MIN_SH, QTA_MSK) | SET_WORD(DLFW_PLE_C2H, QTA_MAX_SH, QTA_MSK);
        if (q == 3) val = SET_WORD(DLFW_PLE_H2C, QTA_MIN_SH, QTA_MSK) | SET_WORD(DLFW_PLE_H2C, QTA_MAX_SH, QTA_MSK);
        io->reg_write32(io->ctx, R_AX_PLE_QTA0_CFG + q * 4, val);
    }

    /* dle_func_en(EN) */
    v = io->reg_read32(io->ctx, R_AX_DMAC_FUNC_EN) | B_DLE_WDE_EN | B_DLE_PLE_EN;
    io->reg_write32(io->ctx, R_AX_DMAC_FUNC_EN, v);

    /* poll WDE then PLE init-ready */
    int ok = 0;
    for (int c = DLE_WAIT_CNT; c; c--) if ((io->reg_read32(io->ctx, R_AX_WDE_INI_STATUS) & WDE_MGN_INI_RDY) == WDE_MGN_INI_RDY) { ok = 1; break; }
    if (!ok) return -2;
    ok = 0;
    for (int c = DLE_WAIT_CNT; c; c--) if ((io->reg_read32(io->ctx, R_AX_PLE_INI_STATUS) & PLE_MGN_INI_RDY) == PLE_MGN_INI_RDY) { ok = 1; break; }
    if (!ok) return -3;
    return 0;
}

/* dmac_pre_init(fwdl=1) for 8852A, then DLE. Call after power-on, before hci_func_en/enable_cpu. */
int rtw_dmac_pre_init(struct rtw_io *io) {
    io->reg_write32(io->ctx, R_AX_DMAC_FUNC_EN, B_MAC_FUNC_EN | B_DMAC_FUNC_EN | B_DISPATCHER_EN | B_PKT_BUF_EN);
    io->reg_write32(io->ctx, R_AX_DMAC_CLK_EN, B_DISPATCHER_CLK_EN);
    return dle_init_dlfw(io);
}

/* hci_func_en (init.c): enable USB HCI TX/RX DMA — now holds, DMAC is on. */
int rtw_hci_func_en(struct rtw_io *io) {
    uint32_t v = io->reg_read32(io->ctx, R_AX_HCI_FUNC_EN) | B_HCI_TXDMA_EN | B_HCI_RXDMA_EN;
    return io->reg_write32(io->ctx, R_AX_HCI_FUNC_EN, v);
}
