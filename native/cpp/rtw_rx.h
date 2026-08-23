#ifndef RTW_RX_H
#define RTW_RX_H
#include <stdint.h>
#include "rtw_io.h"

#define RTW_RPKT_TYPE_WIFI 0

struct rxinfo { int offset; int pktsize; int rpkt_type; int crc_err; int rate; };

/* Strip the rtw89 rxd; fills `out` with the 802.11 frame offset/size + rate/crc. 0 = ok. */
int rtw_rxd_parse(const uint8_t *buf, int len, struct rxinfo *out);

/* Put the MAC into sniffer/monitor mode (written, not yet hardware-verified). */
int rtw_monitor_enable(struct rtw_io *io);

#endif
