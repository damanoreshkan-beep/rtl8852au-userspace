#include <jni.h>
#include <libusb.h>
#include <android/log.h>
#include <atomic>
#include <thread>
#include <unistd.h>
extern "C" {
#include "rtw_io.h"
#include "rtw_rx.h"
}

#define TAG "ax56"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

int  ax56_modeswitch(libusb_device_handle *h);              // modeswitch.cpp   ✅
void ax56_channel_hop(libusb_device_handle *h, std::atomic<bool> &run);  // channel-hop.cpp 🟡
void ax56_parse_frame(const uint8_t *buf, int len);         // frame-parser.cpp ⛔RX

// ported init chain (rtw_pwron.c / rtw_cpu.c / rtw_fwdl.c) — host-verified
extern "C" {
    int     rtw_pwron(struct rtw_io *io, uint8_t cut_msk, void (*delay_us)(uint32_t));
    int     rtw_pwroff(struct rtw_io *io, uint8_t cut_msk, void (*delay_us)(uint32_t));
    uint8_t rtw_read_cut(struct rtw_io *io);
    int     rtw_dmac_pre_init(struct rtw_io *io);   // DMAC_FUNC_EN + DLE(DLFW)
    int     rtw_hci_func_en(struct rtw_io *io);      // HCI TX/RX DMA (needs DMAC on first)
    int     rtw_disable_cpu(struct rtw_io *io);
    int     rtw_enable_cpu(struct rtw_io *io, int dlfw);
    int     rtw_fwdl(struct rtw_io *io, const uint8_t *fw, uint32_t len);
}

// --- rtw_io vtable over libusb (ctx = libusb_device_handle*) ---
// Mirrors the proven usbfs primitives: vendor control 0xC0/0x40 req 0x05, bulk OUT 0x05.
static uint8_t io_r8(void *c, uint16_t a) {
    uint8_t v = 0;
    libusb_control_transfer((libusb_device_handle *)c, 0xC0, 0x05, a, 0, &v, 1, 1000);
    return v;
}
static int io_w8(void *c, uint16_t a, uint8_t v) {
    return libusb_control_transfer((libusb_device_handle *)c, 0x40, 0x05, a, 0, &v, 1, 1000);
}
static uint32_t io_r32(void *c, uint16_t a) {
    uint8_t b[4] = {0};
    libusb_control_transfer((libusb_device_handle *)c, 0xC0, 0x05, a, 0, b, 4, 1000);
    return (uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
static int io_w32(void *c, uint16_t a, uint32_t val) {
    uint8_t b[4] = {(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    return libusb_control_transfer((libusb_device_handle *)c, 0x40, 0x05, a, 0, b, 4, 1000);
}
static int io_tx(void *c, const uint8_t *buf, int len) {
    int n = 0;
    int r = libusb_bulk_transfer((libusb_device_handle *)c, 0x05, (uint8_t *)buf, len, &n, 1000);
    return r == 0 ? n : r;
}
static void io_delay_us(uint32_t us) { usleep(us); }

// The embedded firmware for THIS chip's cut (asset provisioned by the app; see native/fw/).
extern const uint8_t *ax56_fw_blob;   // inner nic blob for the matching cut
extern uint32_t       ax56_fw_len;

static constexpr uint8_t EP_BULK_IN = 0x84, EP_BULK_OUT = 0x05;

struct Driver {
    libusb_context *ctx = nullptr;
    libusb_device_handle *dev = nullptr;
    std::atomic<bool> scanning{false};
    std::thread rxThread, hopThread;
};

static libusb_device_handle *wrap_fd(libusb_context **out, int fd) {
    libusb_context *ctx = nullptr;
    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);  // required on Android
    if (libusb_init(&ctx) != 0) return nullptr;
    libusb_device_handle *h = nullptr;
    int r = libusb_wrap_sys_device(ctx, (intptr_t) fd, &h);
    if (r != 0) { LOGE("wrap_sys_device: %s", libusb_error_name(r)); libusb_exit(ctx); return nullptr; }
    *out = ctx; return h;
}

extern "C" JNIEXPORT jint JNICALL
Java_world_anubis_ax56_UsbController_nativeModeSwitch(JNIEnv *, jobject, jint fd) {
    libusb_context *ctx; libusb_device_handle *h = wrap_fd(&ctx, fd);
    if (!h) return -1;
    int r = ax56_modeswitch(h);
    libusb_close(h); libusb_exit(ctx);
    return r;
}

extern "C" JNIEXPORT jlong JNICALL
Java_world_anubis_ax56_UsbController_nativeInitDriver(JNIEnv *, jobject, jint fd) {
    auto *d = new Driver();
    d->dev = wrap_fd(&d->ctx, fd);
    if (!d->dev) { delete d; return 0; }
    libusb_claim_interface(d->dev, 0);

    struct rtw_io io = { d->dev, io_r8, io_w8, io_r32, io_w32, io_tx };

    // Init chain (host-verified encoders; FIRST on-chip run happens here):
    //   power-on table -> disable/enable CPU (dlfw) -> firmware download.
    uint8_t cut = rtw_read_cut(&io);
    LOGI("chip cut = %u", cut);

    // Recover a warm/dirty chip (0x1e0=0x23, otherwise fwdl-refused) without a physical replug: the full MAC
    // teardown before power-up — the same off->on a kernel rebind does. Harmless on a clean chip; its POLL may
    // no-op on an already-off chip, so its result is not fatal. Phone-validated (hwdriver RECOVER: off->on->fwdl
    // boots STS=7 on a warm chip). This is what lets a session re-attach instead of asking for a replug.
    rtw_pwroff(&io, (uint8_t)(1u << cut), io_delay_us);
    io_delay_us(10000);
    if (rtw_pwron(&io, (uint8_t)(1u << cut), io_delay_us) != 0) {
        LOGE("power-on failed"); goto fail;
    }
    // DMAC + DLE(DLFW) packet-buffer init — lifts H2C_PATH_RDY; then HCI DMA (holds now).
    if (rtw_dmac_pre_init(&io) != 0) { LOGE("dmac/dle init failed"); goto fail; }
    rtw_hci_func_en(&io);
    rtw_disable_cpu(&io);
    if (rtw_enable_cpu(&io, 1) != 0) { LOGE("enable_cpu failed"); goto fail; }

    if (!ax56_fw_blob || !ax56_fw_len) {
        LOGE("no firmware blob provisioned for cut %u (see native/fw/)", cut); goto fail;
    }
    if (rtw_fwdl(&io, ax56_fw_blob, ax56_fw_len) != 0) { LOGE("fwdl failed"); goto fail; }

    LOGI("firmware downloaded; CPU running. MAC/BB/RF init + monitor enable still TODO (§4).");
    // NOTE: returning the handle means fw is up; RX still needs MAC/BB/RF init + monitor enable.
    return reinterpret_cast<jlong>(d);

fail:
    libusb_release_interface(d->dev, 0);
    libusb_close(d->dev); libusb_exit(d->ctx); delete d;
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_world_anubis_ax56_UsbController_nativeStartScan(JNIEnv *, jobject, jlong handle) {
    auto *d = reinterpret_cast<Driver *>(handle);
    if (!d || d->scanning) return -1;

    // Put the MAC into sniffer mode. NOTE: this alone does NOT yield frames — it needs
    // MAC/BB/RF init + a channel set (H2C) first, which are not ported yet (docs/STATUS.md).
    struct rtw_io io = { d->dev, io_r8, io_w8, io_r32, io_w32, io_tx };
    rtw_monitor_enable(&io);

    d->scanning = true;
    d->hopThread = std::thread([d] { ax56_channel_hop(d->dev, d->scanning); });
    d->rxThread = std::thread([d] {
        uint8_t buf[4096];
        while (d->scanning) {
            int n = 0;
            int r = libusb_bulk_transfer(d->dev, EP_BULK_IN, buf, sizeof(buf), &n, 300);
            if (r == 0 && n > 0) ax56_parse_frame(buf, n);   // TODO strip rxdesc first
            else if (r != LIBUSB_ERROR_TIMEOUT) LOGE("bulk_in: %s", libusb_error_name(r));
        }
    });
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_world_anubis_ax56_UsbController_nativeStopScan(JNIEnv *, jobject, jlong handle) {
    auto *d = reinterpret_cast<Driver *>(handle);
    if (!d) return;
    d->scanning = false;
    if (d->rxThread.joinable()) d->rxThread.join();
    if (d->hopThread.joinable()) d->hopThread.join();
}

// Generic TX primitive. Payload built by caller; here only the bulk-OUT write.
// (No deauth/attack helper is provided — see docs/STATUS.md.)
extern "C" JNIEXPORT jint JNICALL
Java_world_anubis_ax56_UsbController_nativeInjectRaw(JNIEnv *env, jobject, jlong handle, jbyteArray frame) {
    auto *d = reinterpret_cast<Driver *>(handle);
    if (!d) return -1;
    jsize n = env->GetArrayLength(frame);
    jbyte *p = env->GetByteArrayElements(frame, nullptr);
    int transferred = 0;
    // TODO: prepend the rtw89 USB txdesc (docs/RESEARCH.md §3a) before the 802.11 frame.
    int r = libusb_bulk_transfer(d->dev, EP_BULK_OUT, reinterpret_cast<uint8_t *>(p), n, &transferred, 500);
    env->ReleaseByteArrayElements(frame, p, JNI_ABORT);
    return r == 0 ? transferred : r;
}

extern "C" JNIEXPORT void JNICALL
Java_world_anubis_ax56_UsbController_nativeClose(JNIEnv *, jobject, jlong handle) {
    auto *d = reinterpret_cast<Driver *>(handle);
    if (!d) return;
    d->scanning = false;
    if (d->rxThread.joinable()) d->rxThread.join();
    if (d->hopThread.joinable()) d->hopThread.join();
    libusb_release_interface(d->dev, 0);
    libusb_close(d->dev); libusb_exit(d->ctx); delete d;
}
