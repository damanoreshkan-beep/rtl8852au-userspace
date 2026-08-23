/* Host test: rtw_rxd_parse locates the 802.11 frame inside a synthetic rxd wrapper. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../cpp/rtw_rx.c"

static void put32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

int main(void){
    int fails = 0;
    uint8_t buf[256]; memset(buf, 0, sizeof(buf));

    /* short rxd (16B), drvsize=1 (=>8B drv_info), shift=2, pktsize=40, rate=0x0b, type=WIFI */
    uint32_t d0 = 0;
    d0 |= 40 & AX_RXD_RPKT_LEN_MSK;                 /* pktsize */
    d0 |= (2u & AX_RXD_SHIFT_MSK) << AX_RXD_SHIFT_SH;
    d0 |= (0u & AX_RXD_RPKT_TYPE_MSK) << AX_RXD_RPKT_TYPE_SH;   /* WIFI */
    d0 |= (1u & AX_RXD_DRV_INFO_SIZE_MSK) << AX_RXD_DRV_INFO_SIZE_SH;
    put32(buf, d0);
    put32(buf + 8, (0x0bu & AX_RXD_RX_DATARATE_MSK) << AX_RXD_RX_DATARATE_SH); /* dword2 rate */
    /* dword3 crc bit clear */

    int expect_off = 16 + 1*8 + 2;  /* = 26 */
    /* place a fake 802.11 beacon start at the expected offset */
    buf[expect_off + 0] = 0x80;     /* beacon frame control */
    int total = expect_off + 40;

    struct rxinfo ri;
    int r = rtw_rxd_parse(buf, total, &ri);

    printf("rtw_rxd_parse -> %d  offset=%d pktsize=%d type=%d crc=%d rate=%d\n",
           r, ri.offset, ri.pktsize, ri.rpkt_type, ri.crc_err, ri.rate);
    #define CHK(c,m) do{ if(!(c)){printf("  FAIL: %s\n",m);fails++;} else printf("  ok: %s\n",m);}while(0)
    CHK(r == 0, "parse succeeded");
    CHK(ri.offset == expect_off, "frame offset = rxdlen(16) + drvsize*8(8) + shift(2) = 26");
    CHK(ri.pktsize == 40, "pktsize decoded = 40");
    CHK(ri.rpkt_type == RXD_RPKT_TYPE_WIFI, "rpkt_type = WIFI(0)");
    CHK(ri.crc_err == 0, "crc_err clear");
    CHK(ri.rate == 0x0b, "rx rate decoded = 0x0b");
    CHK(buf[ri.offset] == 0x80, "frame at offset starts with beacon FC 0x80");

    /* long rxd variant: set LONG bit -> offset uses 32 */
    put32(buf, d0 | AX_RXD_LONG_RXD);
    rtw_rxd_parse(buf, total + 16, &ri);
    CHK(ri.offset == 32 + 8 + 2, "long rxd offset = 42");

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
