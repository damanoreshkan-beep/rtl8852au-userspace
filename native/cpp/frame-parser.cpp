#include <cstdint>
#include <cstdio>
#include <string>
#include <android/log.h>
extern "C" {
#include "rtw_rx.h"
}

#define TAG "ax56"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// 802.11 management header. NOTE: on RX the chip prepends an rtw89 rxdesc that
// must be stripped BEFORE this parser (RSSI/channel/rate live in that rxdesc's
// PHY-status, not in the frame). rxdesc layout is an open item — docs/RESEARCH.md §4.
struct __attribute__((packed)) Dot11Hdr {
    uint16_t fc, duration;
    uint8_t addr1[6], addr2[6], addr3[6];
    uint16_t seq;
};

static std::string mac(const uint8_t *m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// SSID = tag 0 in the tagged-parameters of beacon/probe.
static std::string ssid(const uint8_t *body, int len) {
    int i = 12; // beacon fixed params: timestamp(8)+interval(2)+caps(2)
    while (i + 2 <= len) {
        uint8_t id = body[i], l = body[i + 1];
        if (id == 0 && i + 2 + l <= len)
            return std::string(reinterpret_cast<const char *>(body + i + 2), l);
        i += 2 + l;
    }
    return "<hidden>";
}

// channel = tag 3 (DS Parameter Set).
static int channel(const uint8_t *body, int len) {
    int i = 12;
    while (i + 2 <= len) {
        uint8_t id = body[i], l = body[i + 1];
        if (id == 3 && l >= 1) return body[i + 2];
        i += 2 + l;
    }
    return -1;
}

void ax56_parse_frame(const uint8_t *rxbuf, int rxlen) {
    // Strip the rtw89 rxd wrapper to locate the 802.11 frame (rtw_rx.c, host-tested).
    struct rxinfo ri;
    if (rtw_rxd_parse(rxbuf, rxlen, &ri) != 0) return;
    if (ri.rpkt_type != RTW_RPKT_TYPE_WIFI || ri.crc_err) return;
    const uint8_t *buf = rxbuf + ri.offset;
    int len = ri.pktsize;

    if (len < (int) sizeof(Dot11Hdr)) return;
    auto *h = reinterpret_cast<const Dot11Hdr *>(buf);
    if (((h->fc >> 2) & 3) != 0) return;                 // management only
    uint8_t st = (h->fc >> 4) & 0xf;
    if (st != 0x8 && st != 0x5) return;                  // Beacon / Probe Response
    const uint8_t *body = buf + sizeof(Dot11Hdr);
    int blen = len - sizeof(Dot11Hdr);
    LOGI("BSSID %s ch %d SSID \"%s\"", mac(h->addr3).c_str(), channel(body, blen), ssid(body, blen).c_str());
    // TODO: push {bssid, ssid, channel, rssi} to Kotlin via a JNI callback.
}
