/*
 * rtw_cpu.c — RTL8852AU firmware-CPU enable/disable + cut detection.
 * Ported from lwfinger/rtl8852au @ dwa-x1850 fwdl.c (mac_enable_cpu/mac_disable_cpu)
 * with register addresses from mac_reg.h. All 32-bit RMW + one byte field — the
 * transport we already drive. Runs between power-on and fw download:
 *   rtw_pwron -> rtw_disable_cpu -> rtw_enable_cpu(dlfw=1) -> rtw_fwdl
 */
#include <stdint.h>
#include "rtw_io.h"

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* mac_reg.h addresses */
#define R_AX_SYS_CLK_CTRL     0x0008
#define R_AX_PLATFORM_ENABLE  0x0088
#define R_AX_SYS_CFG1         0x00F0
#define R_AX_HALT_H2C_CTRL    0x0160
#define R_AX_HALT_C2H_CTRL    0x0164
#define R_AX_WCPU_FW_CTRL     0x01E0
#define R_AX_BOOT_REASON      0x01E6
#define R_AX_LDM              0x01E8

#define B_AX_CPU_CLK_EN       BIT(14)
#define B_AX_WCPU_EN          BIT(1)
#define B_AX_PLATFORM_EN      BIT(0)
#define B_AX_WCPU_FWDL_EN     BIT(0)
#define B_AX_H2C_PATH_RDY     BIT(1)
#define B_AX_FWDL_PATH_RDY    BIT(2)
#define B_AX_WCPU_FWDL_STS_SH 5
#define B_AX_WCPU_FWDL_STS_MSK 0x7
#define B_AX_CHIP_VER_SH      12
#define B_AX_CHIP_VER_MSK     0xf
#define B_AX_BOOT_REASON_MSK  0x7
#define FWDL_INITIAL_STATE    0
#define AX_BOOT_REASON_PWR_ON 0

/* cut value from R_AX_SYS_CFG1 (measured on the AX56 = 2 = C-cut). Feed 1<<cut to rtw_pwron. */
uint8_t rtw_read_cut(struct rtw_io *io) {
    return (io->reg_read32(io->ctx, R_AX_SYS_CFG1) >> B_AX_CHIP_VER_SH) & B_AX_CHIP_VER_MSK;
}

static void rmw32(struct rtw_io *io, uint16_t a, uint32_t clr, uint32_t set) {
    uint32_t v = io->reg_read32(io->ctx, a);
    v = (v & ~clr) | set;
    io->reg_write32(io->ctx, a, v);
}

/* mac_disable_cpu (fwdl.c:815) */
int rtw_disable_cpu(struct rtw_io *io) {
    rmw32(io, R_AX_PLATFORM_ENABLE, B_AX_WCPU_EN, 0);
    rmw32(io, R_AX_WCPU_FW_CTRL, B_AX_WCPU_FWDL_EN | B_AX_H2C_PATH_RDY | B_AX_FWDL_PATH_RDY, 0);
    rmw32(io, R_AX_SYS_CLK_CTRL, B_AX_CPU_CLK_EN, 0);
    /* disable_fw_watchdog omitted (SRAM-dbg path); re-toggle PLATFORM_EN */
    rmw32(io, R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN, 0);
    rmw32(io, R_AX_PLATFORM_ENABLE, 0, B_AX_PLATFORM_EN);
    return 0;
}

/* mac_enable_cpu (fwdl.c:754), dlfw=1 for the download path */
int rtw_enable_cpu(struct rtw_io *io, int dlfw) {
    if (io->reg_read32(io->ctx, R_AX_PLATFORM_ENABLE) & B_AX_WCPU_EN)
        return -1;  /* already on */

    io->reg_write32(io->ctx, R_AX_LDM, 0);
    io->reg_write32(io->ctx, R_AX_HALT_H2C_CTRL, 0);
    io->reg_write32(io->ctx, R_AX_HALT_C2H_CTRL, 0);
    rmw32(io, R_AX_SYS_CLK_CTRL, 0, B_AX_CPU_CLK_EN);

    uint32_t v = io->reg_read32(io->ctx, R_AX_WCPU_FW_CTRL);
    v &= ~(B_AX_WCPU_FWDL_EN | B_AX_H2C_PATH_RDY | B_AX_FWDL_PATH_RDY);
    v &= ~((uint32_t)B_AX_WCPU_FWDL_STS_MSK << B_AX_WCPU_FWDL_STS_SH);
    v |= (uint32_t)FWDL_INITIAL_STATE << B_AX_WCPU_FWDL_STS_SH;
    if (dlfw) v |= B_AX_WCPU_FWDL_EN;
    io->reg_write32(io->ctx, R_AX_WCPU_FW_CTRL, v);

    /* boot_reason field lives in one byte (0x01E6[2:0]); PWR_ON=0 -> clear it */
    uint8_t b = io->reg_read8(io->ctx, R_AX_BOOT_REASON);
    b = (b & ~B_AX_BOOT_REASON_MSK) | (AX_BOOT_REASON_PWR_ON & B_AX_BOOT_REASON_MSK);
    io->reg_write8(io->ctx, R_AX_BOOT_REASON, b);

    rmw32(io, R_AX_PLATFORM_ENABLE, 0, B_AX_WCPU_EN);
    /* dlfw=1: CPU_ON reached; fw readiness is checked after fwdl phase2 */
    return 0;
}
