#include <libusb.h>
#include <atomic>
#include <thread>
#include <chrono>

static const int CH_24[] = {1, 6, 11, 2, 7, 3, 8, 4, 9, 5, 10, 12, 13};
static const int CH_5[]  = {36, 40, 44, 48, 149, 153, 157, 161, 165};

// Skeleton hop loop. On rtw89 the channel is set by a firmware H2C command
// (SET_CHANNEL), NOT a raw register write — the actual set call goes in rtw_h2c.cpp
// once fw download works (docs/RESEARCH.md §4). Until then this only paces time.
void ax56_channel_hop(libusb_device_handle *h, std::atomic<bool> &run) {
    using namespace std::chrono;
    while (run) {
        for (int ch : CH_24) { if (!run) return; (void) ch; std::this_thread::sleep_for(milliseconds(250)); }
        for (int ch : CH_5)  { if (!run) return; (void) ch; std::this_thread::sleep_for(milliseconds(250)); }
    }
}
