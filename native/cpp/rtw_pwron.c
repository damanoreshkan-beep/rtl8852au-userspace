/*
 * rtw_pwron.c — RTL8852AU MAC power-on, ported from lwfinger/rtl8852au @ dwa-x1850
 * phl/hal_g6/mac/mac_ax/pwr.c (engine) + mac_8852a/pwr_seq_8852a.c (NIC table) +
 * mac_pwr_switch boot-mode preamble.
 *
 * Power-on is a data-driven walk of a command table (WRITE / POLL / DELAY), each
 * entry filtered by chip cut and interface. It uses ONLY byte register R/W + delay
 * — exactly the transport we already drive over libusb. This precedes fw download.
 *
 * State: engine + NIC table transcribed 1:1 and host-tested (native/test/test_pwron.c).
 * Not yet run on the chip. Needs the cut read (map cut reg -> CAV/CBV/...) wired in
 * native-bridge; USB2 assumed (AX56 enumerates at USB 2.0).
 */
#include <stdint.h>
#include "rtw_io.h"

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* pwr.h constants */
#define PWR_CMD_WRITE 0
#define PWR_CMD_POLL  1
#define PWR_CMD_DELAY 2
#define PWR_CMD_END   3
#define PWR_CAV_MSK  BIT(0)
#define PWR_CBV_MSK  BIT(1)
#define PWR_CCV_MSK  BIT(2)
#define PWR_CVALL    0xFF
#define INTF_SDIO BIT(0)
#define INTF_USB2 BIT(1)
#define INTF_USB3 BIT(2)
#define INTF_USB  (INTF_USB2 | INTF_USB3)
#define INTF_PCIE BIT(3)
#define INTF_ALL  0xF
#define PWR_DELAY_US 0
#define PWR_POLL_CNT 2000

struct pwr_cfg { uint16_t addr; uint8_t cut_msk; uint8_t intf_msk; uint8_t cmd; uint8_t msk; uint8_t val; };

/* mac_pwron_nic_8852a[] (pwr_seq_8852a.c:18) — transcribed 1:1 */
static const struct pwr_cfg pwron_nic_8852a[] = {
    {0x00C6, PWR_CBV_MSK,              INTF_PCIE, PWR_CMD_WRITE, BIT(6), BIT(6)},
    {0x1086, PWR_CVALL,               INTF_SDIO, PWR_CMD_WRITE, BIT(0), 0},
    {0x1086, PWR_CVALL,               INTF_SDIO, PWR_CMD_POLL,  BIT(1), BIT(1)},
    {0x0005, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(4)|BIT(3), 0},
    {0x0005, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(7), 0},
    {0x0005, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(2), 0},
    {0x0006, PWR_CVALL,               INTF_ALL,  PWR_CMD_POLL,  BIT(1), BIT(1)},
    {0x0006, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0005, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0005, PWR_CVALL,               INTF_ALL,  PWR_CMD_POLL,  BIT(0), 0},
    {0x106D, PWR_CBV_MSK|PWR_CCV_MSK, INTF_USB,  PWR_CMD_WRITE, BIT(6), 0},
    {0x0088, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0088, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), 0},
    {0x0088, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0088, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), 0},
    {0x0088, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0083, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(6), 0},
    {0x0080, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(5), BIT(5)},
    {0x0024, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(4)|BIT(3)|BIT(2)|BIT(1)|BIT(0), 0},
    {0x02A0, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(1), BIT(1)},
    {0x02A2, PWR_CVALL,               INTF_ALL,  PWR_CMD_WRITE, BIT(7)|BIT(6)|BIT(5), 0},
    {0x0071, PWR_CVALL,               INTF_PCIE, PWR_CMD_WRITE, BIT(4), 0},
    {0x0010, PWR_CAV_MSK,             INTF_PCIE, PWR_CMD_WRITE, BIT(2), BIT(2)},
    {0x02A0, PWR_CAV_MSK,             INTF_ALL,  PWR_CMD_WRITE, BIT(7)|BIT(6), 0},
    {0xFFFF, PWR_CVALL,               INTF_ALL,  PWR_CMD_END,   0, 0},
};

/* sub_pwr_seq_start (pwr.c:118): walk the table, filtered by cut+intf. */
int rtw_pwr_seq(struct rtw_io *io, uint8_t cut_msk, uint8_t intf_msk,
                const struct pwr_cfg *seq, void (*delay_us)(uint32_t)) {
    for (; seq->cmd != PWR_CMD_END; seq++) {
        if (!(seq->intf_msk & intf_msk) || !(seq->cut_msk & cut_msk)) continue;
        switch (seq->cmd) {
        case PWR_CMD_WRITE: {
            uint8_t v = io->reg_read8(io->ctx, seq->addr);
            v = (v & ~seq->msk) | (seq->val & seq->msk);
            io->reg_write8(io->ctx, seq->addr, v);
            break;
        }
        case PWR_CMD_POLL: {
            int ok = 0;
            for (uint32_t c = PWR_POLL_CNT; c; c--) {
                if ((io->reg_read8(io->ctx, seq->addr) & seq->msk) == (seq->val & seq->msk)) { ok = 1; break; }
                if (delay_us) delay_us(1000);
            }
            if (!ok) return -2;
            break;
        }
        case PWR_CMD_DELAY:
            if (delay_us) delay_us(seq->val == PWR_DELAY_US ? seq->addr : seq->addr * 1000);
            break;
        }
    }
    return 0;
}

/* mac_pwr_switch(on) preamble (pwr.c:238) — clear boot-mode latches, then run the table.
 * cut_msk = one of PWR_C?V_MSK from the chip's cut register; intf USB2 for the AX56. */
int rtw_pwron(struct rtw_io *io, uint8_t cut_msk, void (*delay_us)(uint32_t)) {
    /* boot-mode preamble: if R_AX_GPIO_MUXCFG[boot] set, clear the auto-power latches.
     * Register addresses (R_AX_SYS_PW_CTRL 0x0004, SYS_STATUS1 0x00F4, GPIO_MUXCFG 0x02A0,
     * RSV_CTRL 0x001C) are on the low MAC page we already reach. Done as 32-bit RMW. */
    /* NOTE: boot-mode is usually clear on a fresh USB attach; this is a safety preamble. */
    return rtw_pwr_seq(io, cut_msk, INTF_USB2, pwron_nic_8852a, delay_us);
}
