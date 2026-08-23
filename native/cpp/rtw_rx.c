/*
 * rtw_rx.c — RTL8852AU RX descriptor strip + monitor(sniffer) enable.
 * Ported from lwfinger/rtl8852au @ dwa-x1850 rxdesc.h + trx_desc.c (rxdes_parse_comm)
 * + mac_reg.h (R_AX_RX_FLTR_OPT / B_AX_SNIFFER_MODE).
 *
 * On bulk IN each transfer is [rxd][drv_info][shift pad][802.11 frame]. This strips the
 * rxd to locate the frame; frame-parser then reads the 802.11 header.
 *
 * Verified: the rxd-offset math is host-tested (native/test/test_rxd.c). Monitor enable
 * and RSSI-from-PPDU-status are written but need on-hardware confirmation (no RX yet).
 */
#include <stdint.h>
#include "rtw_io.h"
#include "rtw_rx.h"

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* rxdesc.h dword0 fields */
#define AX_RXD_RPKT_LEN_MSK   0x3fff
#define AX_RXD_SHIFT_SH       14
#define AX_RXD_SHIFT_MSK      0x3
#define AX_RXD_RPKT_TYPE_SH   24
#define AX_RXD_RPKT_TYPE_MSK  0xf
#define AX_RXD_DRV_INFO_SIZE_SH  28
#define AX_RXD_DRV_INFO_SIZE_MSK 0x7
#define AX_RXD_LONG_RXD       BIT(31)
#define RXD_SHORT_LEN 16   /* sizeof(rxd_short_t) = 4*u32 */
#define RXD_LONG_LEN  32   /* sizeof(rxd_long_t)  = 8*u32 */
#define AX_RXD_CRC32_ERR      BIT(9)   /* dword3 */
#define RXD_RPKT_TYPE_WIFI    0
/* dword2: rx data rate */
#define AX_RXD_RX_DATARATE_SH  16
#define AX_RXD_RX_DATARATE_MSK 0x1ff

/* mac_reg.h — sniffer / RX filter */
#define R_AX_RX_FLTR_OPT   0xCE20
#define B_AX_SNIFFER_MODE  BIT(0)
#define B_AX_A_A1_MATCH    BIT(1)
#define B_AX_A_BC          BIT(2)
#define B_AX_A_MC          BIT(3)

static uint32_t le32p(const uint8_t *p) {
    return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Parse the rxd; returns 0 and fills out on success, -1 if the buffer is too small. */
int rtw_rxd_parse(const uint8_t *buf, int len, struct rxinfo *out) {
    if (len < RXD_SHORT_LEN) return -1;
    uint32_t d0 = le32p(buf);
    int rxdlen = (d0 & AX_RXD_LONG_RXD) ? RXD_LONG_LEN : RXD_SHORT_LEN;
    int drvsize = (d0 >> AX_RXD_DRV_INFO_SIZE_SH) & AX_RXD_DRV_INFO_SIZE_MSK; /* ×8 bytes */
    int shift   = (d0 >> AX_RXD_SHIFT_SH) & AX_RXD_SHIFT_MSK;
    out->pktsize   = d0 & AX_RXD_RPKT_LEN_MSK;
    out->rpkt_type = (d0 >> AX_RXD_RPKT_TYPE_SH) & AX_RXD_RPKT_TYPE_MSK;
    out->offset    = rxdlen + drvsize * 8 + shift;   /* start of the 802.11 frame */
    out->crc_err   = (len >= 16) ? !!(le32p(buf + 12) & AX_RXD_CRC32_ERR) : 0;
    out->rate      = (len >= 12) ? (int)((le32p(buf + 8) >> AX_RXD_RX_DATARATE_SH) & AX_RXD_RX_DATARATE_MSK) : 0;
    if (out->offset + out->pktsize > len) return -1;
    return 0;
}

/* Put the MAC into sniffer/monitor mode: accept all frames on band 0.
 * WRITTEN, NOT on-hardware verified — exact accept-filter tuning wants a real capture. */
int rtw_monitor_enable(struct rtw_io *io) {
    uint32_t v = io->reg_read32(io->ctx, R_AX_RX_FLTR_OPT);
    v |= B_AX_SNIFFER_MODE | B_AX_A_A1_MATCH | B_AX_A_BC | B_AX_A_MC;
    return io->reg_write32(io->ctx, R_AX_RX_FLTR_OPT, v);
}
