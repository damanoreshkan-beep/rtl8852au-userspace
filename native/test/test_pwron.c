/* Host test: walk the 8852A NIC power-on table through rtw_pwr_seq with a fake chip,
 * verifying cut/intf filtering, command decode, order, and poll handling. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../cpp/rtw_pwron.c"

static uint8_t reg[0x10000];
static int nwrite = 0;
static uint16_t wr_addr[64]; static int wr_n = 0;

static uint8_t r8(void *c, uint16_t a) {
    (void)c;
    if (a == 0x0005) return 0x00;   /* poll wants bit0==0 */
    if (a == 0x0006) return 0xFF;   /* poll wants bit1==1 */
    return reg[a];
}
static int w8(void *c, uint16_t a, uint8_t v) { (void)c; reg[a]=v; if (wr_n<64) wr_addr[wr_n++]=a; nwrite++; return 0; }
static void nodelay(uint32_t us) { (void)us; }

#define CHK(cond,msg) do{ if(!(cond)){printf("  FAIL: %s\n",msg);fails++;} else printf("  ok: %s\n",msg);}while(0)

int main(void) {
    int fails = 0;
    struct rtw_io io = { .ctx=NULL, .reg_read8=r8, .reg_write8=w8 };
    /* cut = CBV (a real 8852A cut), interface USB2 */
    int r = rtw_pwr_seq(&io, PWR_CBV_MSK, INTF_USB2, pwron_nic_8852a, nodelay);

    printf("rtw_pwr_seq -> %d, writes=%d\n", r, nwrite);
    CHK(r == 0, "power-on table completed (both polls satisfied)");
    CHK(nwrite == 16, "exactly 16 register writes executed for USB2/CBV (PCIE/SDIO/CAV filtered out)");
    /* spot-check order: first executed write is 0x0005, USB-only 0x106D is present, no PCIE addr */
    CHK(wr_addr[0] == 0x0005, "first write targets 0x0005");
    int has106D = 0, hasPCIE = 0;
    for (int i = 0; i < wr_n; i++) {
        if (wr_addr[i] == 0x106D) has106D = 1;
        if (wr_addr[i] == 0x00C6 || wr_addr[i] == 0x0071 || wr_addr[i] == 0x0010) hasPCIE = 1;
    }
    CHK(has106D, "USB-only entry 0x106D executed");
    CHK(!hasPCIE, "no PCIE-only entry (0x00C6/0x0071/0x0010) executed");

    /* --- power-OFF table: the warm-chip teardown that RECOVER runs before power-on --- */
    nwrite = 0; wr_n = 0;
    int ro = rtw_pwr_seq(&io, PWR_CCV_MSK, INTF_USB2, pwroff_nic_8852a, nodelay);
    printf("pwroff rtw_pwr_seq -> %d, writes=%d\n", ro, nwrite);
    CHK(ro == 0, "power-off table completed (0x0005 bit1 poll satisfied)");
    CHK(nwrite == 9, "9 teardown writes for USB2/CCV (PCIE/SDIO filtered out)");
    int hasF0 = 0, has07 = 0;
    for (int i = 0; i < wr_n; i++) { if (wr_addr[i] == 0x02F0) hasF0 = 1; if (wr_addr[i] == 0x0007) has07 = 1; }
    CHK(hasF0 && has07, "teardown hits 0x02F0 (fwdl-state reset) and 0x0007 (USB disable)");

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
