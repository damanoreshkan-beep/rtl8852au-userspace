#include <libusb.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// usb_modeswitch StandardEject as plain BOT CBWs. LUN0 pair is enough — the LUN1
// pair returns EPROTO because the device is already detaching (that is success).
// Verified on the AX56 this session (storage 0bda:1a2b -> wifi 0b05:1997).
static const char *MSG[] = {
    "5553424387654321000000000000061e000000000000000000000000000000", // ALLOW MEDIUM REMOVAL
    "5553424397654321000000000000061b000000020000000000000000000000", // START STOP UNIT, LoEj
};
static constexpr uint8_t EP_OUT = 0x05, EP_IN = 0x84;

static int hex2bin(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) { char b[3] = {s[0], s[1], 0}; out[n++] = (uint8_t) strtol(b, nullptr, 16); }
    return n;
}

int ax56_modeswitch(libusb_device_handle *h) {
    libusb_set_auto_detach_kernel_driver(h, 1);   // kick usb-storage off interface 0
    if (libusb_claim_interface(h, 0) != 0) return -1;
    uint8_t cbw[31]; int tr;
    for (auto *m : MSG) {
        int len = hex2bin(m, cbw);
        libusb_bulk_transfer(h, EP_OUT, cbw, len, &tr, 2000);
        uint8_t csw[13];
        libusb_bulk_transfer(h, EP_IN, csw, sizeof(csw), &tr, 2000);
    }
    libusb_release_interface(h, 0);
    return 0;
}
