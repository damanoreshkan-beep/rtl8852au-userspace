/* Host test: rtw_enable_cpu / rtw_disable_cpu / rtw_read_cut against a fake chip. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../cpp/rtw_cpu.c"

static uint8_t reg8map[0x10000];
static uint32_t reg32map[0x10000];

static uint8_t r8(void *c, uint16_t a){ (void)c; return reg8map[a]; }
static int w8(void *c, uint16_t a, uint8_t v){ (void)c; reg8map[a]=v; return 0; }
static uint32_t r32(void *c, uint16_t a){ (void)c; return reg32map[a]; }
static int w32(void *c, uint16_t a, uint32_t v){ (void)c; reg32map[a]=v; return 0; }

#define CHK(cond,msg) do{ if(!(cond)){printf("  FAIL: %s\n",msg);fails++;} else printf("  ok: %s\n",msg);}while(0)

int main(void){
    int fails = 0;
    struct rtw_io io = { .ctx=NULL, .reg_read8=r8, .reg_write8=w8, .reg_read32=r32, .reg_write32=w32 };

    /* cut detection: emulate the real on-device read 0x00F0 = 0x0c492537 -> cut 2 */
    reg32map[0x00F0] = 0x0c492537;
    uint8_t cut = rtw_read_cut(&io);
    printf("read_cut -> %u (expect 2 = C-cut)\n", cut);
    CHK(cut == 2, "cut decoded from R_AX_SYS_CFG1 = 2 (matches on-device read)");

    /* enable_cpu: platform initially off (WCPU_EN clear) */
    reg32map[0x0088] = 0;
    int r = rtw_enable_cpu(&io, 1);
    CHK(r == 0, "rtw_enable_cpu(dlfw=1) succeeded");
    CHK(reg32map[0x0008] & (1u<<14), "CPU_CLK_EN set in SYS_CLK_CTRL");
    CHK(reg32map[0x01E0] & 1u,       "WCPU_FWDL_EN set in WCPU_FW_CTRL (dlfw)");
    CHK(((reg32map[0x01E0]>>5)&7) == 0, "FWDL_STS reset to initial (0)");
    CHK(reg32map[0x0088] & (1u<<1),  "WCPU_EN set in PLATFORM_ENABLE (CPU on)");
    CHK((reg8map[0x01E6] & 7) == 0,  "boot_reason = PWR_ON (0)");

    /* enable when already on -> refuse */
    r = rtw_enable_cpu(&io, 1);
    CHK(r == -1, "enable_cpu refuses when WCPU_EN already set");

    /* disable_cpu clears WCPU_EN */
    rtw_disable_cpu(&io);
    CHK(!(reg32map[0x0088] & (1u<<1)), "WCPU_EN cleared after disable_cpu");
    CHK(reg32map[0x0088] & 1u,         "PLATFORM_EN re-asserted after disable_cpu");

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
