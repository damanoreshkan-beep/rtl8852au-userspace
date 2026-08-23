/*
 * rtw_fwdl.c — RTL8852AU firmware download, ported to the two userspace primitives
 * we already drive over libusb (register R/W via control transfers, TX packet via
 * bulk OUT). Faithful transcription of lwfinger/rtl8852au @ dwa-x1850
 * phl/hal_g6/mac/mac_ax/fwdl.c (mac_fwdl) + fwcmd.c (h2c_pkt_set_hdr_fwdl).
 *
 * State of this file: the parse, the header/H2C encoding and the phase0/1/2 state
 * machine are CONCRETE (all constants resolved from source — see docs/RESEARCH.md).
 * The ONE remaining leaf is build_txdesc() — the WiFi TX descriptor that wraps each
 * H2C packet for the bulk-OUT FIFO. It is isolated and clearly stubbed, not faked.
 *
 * NOT yet runnable end-to-end: it also needs the power-on + mac_enable_fw (CPU_ON)
 * that precede fwdl. See docs/STATUS.md.
 */
#include <stdint.h>
#include <string.h>
#include "rtw_io.h"   /* shared transport vtable */

/* ---- resolved constants (fwdl.h / fwcmd.h / fwcmd_intf.h) ---- */
#define R_AX_WCPU_FW_CTRL      0x01E0
#define B_H2C_PATH_RDY         (1u << 1)   /* 0x1E0[1] */
#define B_FWDL_PATH_RDY        (1u << 2)   /* 0x1E0[2] */
#define FWDL_STS_SH            5           /* 0x1E0[7:5] */
#define FWDL_STS_MSK          0x7
#define FWDL_WCPU_FW_INIT_RDY  7
#define R_AX_HALT_H2C_CTRL     0x0160      /* confirmed mac_reg.h */
#define R_AX_HALT_C2H_CTRL     0x0164      /* confirmed mac_reg.h */

#define FWHDR_HDR_LEN          32          /* sizeof(fwhdr_hdr_t)  = 8*u32 */
#define FWHDR_SECTION_LEN      16          /* sizeof(fwhdr_section_t) = 4*u32 */
#define FWDL_SECTION_PER_PKT_LEN 2020
#define FWDL_SECTION_CHKSUM_LEN  8
#define SEC_INFO_CHECKSUM     (1u << 28)
#define SEC_INFO_REDL         (1u << 29)
#define FWCMD_HDR_LEN          8

/* fwcmd_hdr.hdr0 fields (fwcmd_intf.h) */
#define H2C_CAT_MAC     0x1
#define H2C_CL_FWDL     0x3
#define H2C_FUNC_FWHDR_DL 0x0
#define H2C_TYPE        0x0                /* FWCMD_TYPE_H2C */

/* txdesc = wd_body_t = 6*u32 = 24 B (trx_desc.c txdes_proc_h2c_fwdl; type.h/txdesc.h) */
#define WD_BODY_LEN     24
#define MAC_AX_DMA_H2C  12                 /* enum mac_ax_dma_ch index */
#define AX_TXD_CH_DMA_SH   16
#define AX_TXD_FWDL_EN     (1u << 20)
#define AX_TXD_TXPKTSIZE_MSK 0x3fff
#define SECTION_PUSH_LEN 8                 /* __sections_push reserves 8 B before a section chunk */

#define FWDL_WAIT_CNT   400000

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void wr_le32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

struct sec_info { const uint8_t *addr; uint32_t len; uint32_t dladdr; int redl; };
struct bin_info { uint8_t section_num; uint32_t hdr_len; struct sec_info sec[64]; };

/* fwhdr_parser (fwdl.c:84) — layout: [hdr 32B][sec desc 16B * N][bodies...] */
static int fwhdr_parse(const uint8_t *fw, uint32_t len, struct bin_info *info) {
    info->section_num = (le32(fw + 6*4) >> 8) & 0xff;               /* dword6[15:8] */
    info->hdr_len = FWHDR_HDR_LEN + info->section_num * FWHDR_SECTION_LEN;
    const uint8_t *desc = fw + FWHDR_HDR_LEN;
    const uint8_t *body = fw + info->hdr_len;
    for (int i = 0; i < info->section_num; i++) {
        uint32_t d0 = le32(desc + 0), d1 = le32(desc + 4);
        uint32_t sz = d1 & 0xFFFFFF;                                /* SEC_SIZE */
        if (d1 & SEC_INFO_CHECKSUM) sz += FWDL_SECTION_CHKSUM_LEN;
        info->sec[i].dladdr = d0 & 0x1FFFFFFF;
        info->sec[i].len = sz;
        info->sec[i].redl = (d1 & SEC_INFO_REDL) ? 1 : 0;
        info->sec[i].addr = body;
        body += sz;
        desc += FWHDR_SECTION_LEN;
    }
    return (body == fw + len) ? 0 : -1;   /* same integrity check the driver enforces */
}

/* fwcmd_hdr (8B) for FWHDR_DL: cat|class<<2|func<<8|type<<16|seq<<24 ; hdr1=len&0x3fff */
static void build_fwcmd_hdr(uint8_t *out, uint32_t data_len, uint8_t seq) {
    uint32_t hdr0 = (H2C_CAT_MAC & 0x3)
                  | ((H2C_CL_FWDL & 0x3f) << 2)
                  | ((H2C_FUNC_FWHDR_DL & 0xff) << 8)
                  | ((H2C_TYPE & 0xf) << 16)
                  | ((uint32_t)seq << 24);
    uint32_t hdr1 = data_len & 0x3fff;    /* rack=dack=0 for fwdl */
    wr_le32(out + 0, hdr0);
    wr_le32(out + 4, hdr1);
}

/* WiFi TX descriptor (wd_body_t, 24 B) — resolved from trx_desc.c txdes_proc_h2c_fwdl.
 * is_fwdl: 0 for the H2C fw-header packet, 1 for section packets (sets AX_TXD_FWDL_EN). */
static void build_txdesc(uint8_t *out, uint32_t pktlen, int is_fwdl) {
    memset(out, 0, WD_BODY_LEN);
    uint32_t d0 = ((uint32_t)MAC_AX_DMA_H2C << AX_TXD_CH_DMA_SH) | (is_fwdl ? AX_TXD_FWDL_EN : 0);
    wr_le32(out + 0,  d0);                              /* dword0 */
    wr_le32(out + 8,  pktlen & AX_TXD_TXPKTSIZE_MSK);   /* dword2 */
    /* dword1,3,4,5 = 0 */
}

/* one packet onto bulk OUT: [txdesc 24B][payload]. pktlen field = payload length. */
static int tx_packet(struct rtw_io *io, const uint8_t *payload, int plen, int is_fwdl) {
    uint8_t pkt[WD_BODY_LEN + FWCMD_HDR_LEN + FWDL_SECTION_PER_PKT_LEN + 8];
    if (WD_BODY_LEN + plen > (int)sizeof(pkt)) return -2;
    build_txdesc(pkt, (uint32_t)plen, is_fwdl);
    memcpy(pkt + WD_BODY_LEN, payload, plen);
    return io->tx(io->ctx, pkt, WD_BODY_LEN + plen);
}

static int poll_reg(struct rtw_io *io, uint16_t addr, uint32_t mask, uint32_t want) {
    for (uint32_t c = FWDL_WAIT_CNT; c; c--)
        if ((io->reg_read32(io->ctx, addr) & mask) == want) return 0;
    return -1;
}

/* mac_fwdl phases (fwdl.c:449/475/549). Precondition: CPU_ON (mac_enable_fw done). */
int rtw_fwdl(struct rtw_io *io, const uint8_t *fw, uint32_t len) {
    struct bin_info info;
    if (fwhdr_parse(fw, len, &info) != 0) return -1;

    /* phase0: H2C path ready */
    if (poll_reg(io, R_AX_WCPU_FW_CTRL, B_H2C_PATH_RDY, B_H2C_PATH_RDY)) return -2;

    /* phase1: send fw header (hdr_len bytes), patch dword7 FW_PART_SZ=2020 first */
    {
        uint8_t hdr[FWHDR_HDR_LEN + 64*FWHDR_SECTION_LEN];
        memcpy(hdr, fw, info.hdr_len);
        uint32_t d7 = le32(hdr + 7*4);
        d7 = (d7 & ~0xffffu) | (FWDL_SECTION_PER_PKT_LEN & 0xffff);
        wr_le32(hdr + 7*4, d7);
        uint8_t payload[FWCMD_HDR_LEN + sizeof(hdr)];
        build_fwcmd_hdr(payload, info.hdr_len, 0);
        memcpy(payload + FWCMD_HDR_LEN, hdr, info.hdr_len);
        /* header packet is an H2C command (is_fwdl=0) */
        if (tx_packet(io, payload, FWCMD_HDR_LEN + info.hdr_len, 0)) return -3;
    }
    if (poll_reg(io, R_AX_WCPU_FW_CTRL, B_FWDL_PATH_RDY, B_FWDL_PATH_RDY)) return -4;
    io->reg_write32(io->ctx, R_AX_HALT_H2C_CTRL, 0);
    io->reg_write32(io->ctx, R_AX_HALT_C2H_CTRL, 0);

    /* phase2: send each section body, chunked at 2020B.
     * Section packet = [txdesc(FWDL)][8B __sections_push][chunk] (is_fwdl=1). The 8-byte
     * prefix is reserved by __sections_push; its exact contents want confirming against a
     * real capture — zero-filled here. */
    for (int i = 0; i < info.section_num; i++) {
        const uint8_t *p = info.sec[i].addr;
        uint32_t rem = info.sec[i].len;
        while (rem) {
            uint32_t chunk = rem > FWDL_SECTION_PER_PKT_LEN ? FWDL_SECTION_PER_PKT_LEN : rem;
            uint8_t payload[SECTION_PUSH_LEN + FWDL_SECTION_PER_PKT_LEN];
            memset(payload, 0, SECTION_PUSH_LEN);
            memcpy(payload + SECTION_PUSH_LEN, p, chunk);
            if (tx_packet(io, payload, (int)(SECTION_PUSH_LEN + chunk), 1)) return -5;
            p += chunk; rem -= chunk;
        }
    }
    /* check_fw_rdy: 0x1E0[7:5] == 7 */
    if (poll_reg(io, R_AX_WCPU_FW_CTRL, (uint32_t)FWDL_STS_MSK << FWDL_STS_SH,
                 (uint32_t)FWDL_WCPU_FW_INIT_RDY << FWDL_STS_SH)) return -6;
    return 0;
}
