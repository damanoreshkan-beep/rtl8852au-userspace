/* Shared transport vtable for the RTL8852AU userspace port.
 * Filled by native-bridge.cpp with our libusb primitives:
 *   reg_*  -> vendor control transfer (0xC0/0x40, bRequest 0x05, wValue=addr, len=1/2/4)
 *   tx     -> bulk OUT (EP 0x05)
 * Register access is proven on hardware; see docs/RESEARCH.md. */
#ifndef RTW_IO_H
#define RTW_IO_H
#include <stdint.h>

struct rtw_io {
    void *ctx;
    uint8_t  (*reg_read8)(void *ctx, uint16_t addr);
    int      (*reg_write8)(void *ctx, uint16_t addr, uint8_t v);
    uint32_t (*reg_read32)(void *ctx, uint16_t addr);
    int      (*reg_write32)(void *ctx, uint16_t addr, uint32_t v);
    int      (*tx)(void *ctx, const uint8_t *buf, int len);   /* bulk OUT */
};

#endif
