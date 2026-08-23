/* Host test: exercise rtw_fwdl.c's encoder against the real U2-nic fw blob and
 * assert the exact bytes of the txdesc + fwcmd header + section chunking. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../cpp/rtw_fwdl.c"

static uint32_t fake_read(void *c, uint16_t a) { (void)c; (void)a; return 0xFFFFFFFF; } /* all polls ready */
static int fake_write(void *c, uint16_t a, uint32_t v) { (void)c;(void)a;(void)v; return 0; }

static int npkt = 0, fails = 0;
static long total_bytes = 0;
static uint8_t first_pkt[64]; static int first_len = 0;
static int max_pkt = 0;

static int rec_tx(void *c, const uint8_t *b, int len) {
    (void)c;
    if (npkt == 0) { first_len = len < 64 ? len : 64; memcpy(first_pkt, b, first_len); }
    if (len > max_pkt) max_pkt = len;
    /* every section packet (npkt>0) must have FWDL_EN in txdesc dword0 */
    if (npkt > 0) {
        uint32_t d0 = b[0] | b[1]<<8 | b[2]<<16 | (uint32_t)b[3]<<24;
        if (!(d0 & (1u<<20))) { printf("  FAIL pkt %d missing FWDL_EN (d0=%08x)\n", npkt, d0); fails++; }
        if (((d0>>16)&0xf) != 12) { printf("  FAIL pkt %d wrong DMA ch\n", npkt); fails++; }
    }
    npkt++; total_bytes += len;
    return 0;
}

#define CHK(cond, msg) do{ if(!(cond)){ printf("  FAIL: %s\n", msg); fails++; } else printf("  ok: %s\n", msg);}while(0)

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);

    /* mfw entry1 = cv=U2 nic: off=0x5e890 size=386304 (from validator) */
    uint32_t off = 0x5e890, len = 386304;
    printf("== encoding U2-nic blob off=0x%x len=%u ==\n", off, len);

    struct rtw_io io = { .ctx=NULL, .reg_read32=fake_read, .reg_write32=fake_write, .tx=rec_tx };
    int r = rtw_fwdl(&io, buf + off, len);

    printf("rtw_fwdl returned %d, packets=%d, total_tx=%ld, max_pkt=%d\n", r, npkt, total_bytes, max_pkt);

    CHK(r == 0, "rtw_fwdl completed all phases");
    /* first packet = header: [txdesc 24][fwcmd_hdr 8][fw header] */
    uint32_t d0 = first_pkt[0] | first_pkt[1]<<8 | first_pkt[2]<<16 | (uint32_t)first_pkt[3]<<24;
    uint32_t psz = first_pkt[8] | first_pkt[9]<<8;  /* dword2 low */
    uint32_t hdr0 = first_pkt[24] | first_pkt[25]<<8 | first_pkt[26]<<16 | (uint32_t)first_pkt[27]<<24;
    printf("  header txdesc d0=0x%08x pktsize=%u  fwcmd hdr0=0x%08x\n", d0, psz, hdr0);
    CHK(((d0>>16)&0xf)==12, "header txdesc DMA channel = H2C(12)");
    CHK(!(d0 & (1u<<20)), "header txdesc FWDL_EN clear (it is an H2C cmd)");
    CHK(hdr0 == 0x0000000D, "fwcmd hdr0 = 0x0D (cat=MAC,class=FWDL,func=FWHDR_DL)");
    /* header hdr_len for section_num=3 = 32+48 = 80; pktsize = 8 + 80 = 88 */
    CHK(psz == 88, "header packet pktsize = fwcmd(8) + hdr_len(80) = 88");
    CHK(max_pkt <= 24 + 8 + 2020, "no packet exceeds txdesc+push+2020 chunk");
    CHK(fails == 0, "all per-packet FWDL_EN/channel checks");

    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
