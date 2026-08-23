/* Host test: DLFW dle config consistency + the dmac_pre_init/dle register writes. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../cpp/rtw_dle.c"

static uint32_t reg[0x10000];
static uint32_t r32(void *c, uint16_t a) {
    (void)c;
    if (a == R_AX_WDE_INI_STATUS || a == R_AX_PLE_INI_STATUS) return 0x3; /* fake ready */
    return reg[a];
}
static int w32(void *c, uint16_t a, uint32_t v) { (void)c; reg[a] = v; return 0; }
static uint8_t r8(void *c, uint16_t a) { (void)c; return reg[a] & 0xff; }
static int w8(void *c, uint16_t a, uint8_t v) { (void)c; reg[a] = v; return 0; }

#define CHK(cond,msg) do{ if(!(cond)){printf("  FAIL: %s\n",msg);fails++;} else printf("  ok: %s\n",msg);}while(0)

int main(void) {
    int fails = 0;
    struct rtw_io io = { .ctx=NULL, .reg_read8=r8, .reg_write8=w8, .reg_read32=r32, .reg_write32=w32 };

    /* 1) the internal-consistency invariant the driver enforces */
    printf("used_size=%u fifo_size=%u\n", rtw_dle_used_size(), rtw_dle_fifo_size());
    CHK(rtw_dle_used_size() == rtw_dle_fifo_size(), "DLFW used_size == fifo_size (458752) — config consistent");
    CHK(rtw_dle_used_size() == 458752, "used_size = 64*4096 + 128*(64+1472) = 458752");

    /* 2) run the init and inspect the register writes */
    int r = rtw_dmac_pre_init(&io);
    CHK(r == 0, "rtw_dmac_pre_init completed (INI ready polls passed)");

    uint32_t dfe = reg[R_AX_DMAC_FUNC_EN];
    CHK((dfe & (B_MAC_FUNC_EN|B_DMAC_FUNC_EN|B_DISPATCHER_EN|B_PKT_BUF_EN)) ==
        (B_MAC_FUNC_EN|B_DMAC_FUNC_EN|B_DISPATCHER_EN|B_PKT_BUF_EN), "DMAC_FUNC_EN has MAC|DMAC|DISPATCHER|PKT_BUF");
    CHK((dfe & (B_DLE_WDE_EN|B_DLE_PLE_EN)) == (B_DLE_WDE_EN|B_DLE_PLE_EN), "DLE WDE+PLE enabled at end");
    CHK(reg[R_AX_DMAC_CLK_EN] & B_DISPATCHER_CLK_EN, "DMAC_CLK_EN has DISPATCHER_CLK");

    uint32_t wpc = reg[R_AX_WDE_PKTBUF_CFG];
    CHK(((wpc >> PAGE_SEL_SH) & PAGE_SEL_MSK) == SEL_WDE_64, "WDE page_sel = 64");
    CHK(((wpc >> FREE_PAGE_NUM_SH) & FREE_PAGE_NUM_MSK) == WDE_LNK, "WDE free_page = 0");

    uint32_t ppc = reg[R_AX_PLE_PKTBUF_CFG];
    CHK(((ppc >> PAGE_SEL_SH) & PAGE_SEL_MSK) == SEL_PLE_128, "PLE page_sel = 128");
    CHK(((ppc >> START_BOUND_SH) & START_BOUND_MSK) == 32, "PLE start_bound = (4096*64)/8192 = 32");
    CHK(((ppc >> FREE_PAGE_NUM_SH) & FREE_PAGE_NUM_MSK) == PLE_LNK, "PLE free_page = 64");

    uint32_t q3 = reg[R_AX_PLE_QTA0_CFG + 3*4];
    CHK((q3 & QTA_MSK) == DLFW_PLE_H2C && ((q3 >> QTA_MAX_SH) & QTA_MSK) == DLFW_PLE_H2C, "PLE Q3 (h2c) min=max=48");
    uint32_t q2 = reg[R_AX_PLE_QTA0_CFG + 2*4];
    CHK((q2 & QTA_MSK) == DLFW_PLE_C2H, "PLE Q2 (c2h) = 16");
    CHK(reg[R_AX_PLE_QTA0_CFG + 0*4] == 0 && reg[R_AX_WDE_QTA0_CFG] == 0, "other quotas zero");

    /* 3) hci_func_en holds now that DMAC is on */
    rtw_hci_func_en(&io);
    CHK((reg[R_AX_HCI_FUNC_EN] & (B_HCI_TXDMA_EN|B_HCI_RXDMA_EN)) == (B_HCI_TXDMA_EN|B_HCI_RXDMA_EN), "HCI TX+RX DMA enabled");

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
