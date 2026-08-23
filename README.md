# rtl8852au-userspace

**No-root userspace Wi-Fi control for the Realtek RTL8852AU (ASUS USB-AX56 / D-Link DWA-X1850 /
Alfa AWUS036AXER) on Android — driving the chip over `libusb` with zero kernel modules.**

The goal: **monitor mode + raw 802.11 capture** on a Wi-Fi 6 USB adapter from an unrooted Android phone,
by porting Realtek's HALMAC bring-up sequence into user space on top of `libusb_wrap_sys_device()`.

> **Status: research / work-in-progress.** The USB transport, mode-switch and register access are proven on
> real hardware; the firmware-download + monitor path is an in-progress port. See
> [`docs/STATUS.md`](docs/STATUS.md) for the exact per-feature state. This repo documents a real bring-up as
> it happens — not a finished driver (yet).

## Why this exists

The RTL8852AU has **no userspace driver**. [OpenIPC/devourer](https://github.com/OpenIPC/devourer) — the
mature userspace Realtek driver — explicitly excludes the 8852A family. The in-kernel `rtw89` driver needs
root, a recent kernel and a kernel module. So on an unrooted Android phone there is currently *no* way to put
this adapter into monitor mode. This project is the attempt to change that, entirely in user space.

The approach mirrors devourer's: own `libusb`, wrap the Android USB file descriptor, and replay the vendor
driver's MAC/BB/RF bring-up — but for the 8852A HALMAC generation that devourer skips.

## How the no-root path works

1. Android's USB Host API hands out a file descriptor for the device (via Termux:API `termux-usb`).
2. `libusb_set_option(NO_DEVICE_DISCOVERY)` + `libusb_wrap_sys_device(fd)` adopts it — no root, no `/dev/bus/usb`.
3. The adapter first enumerates as a **read-only "Driver Storage" disk** (`0bda:1a2b`). A SCSI eject
   (usb_modeswitch's StandardEject, sent as raw Bulk-Only CBWs) flips it to Wi-Fi mode (`0b05:1997`).
4. From there: vendor **control transfers** (`0xC0/0x40`, bRequest `0x05`) read/write MAC registers, and
   **bulk OUT** carries H2C / firmware / TX packets — the same two primitives the kernel driver uses.

## What's in here

```
tool/        Deno + termux-usb control tool — WORKS TODAY, no root
  ax56ctl.sh   list · id · switch(storage→wifi) · reg read/write
native/      Android Studio app scaffold (Kotlin + NDK/libusb) — the UI vehicle
  cpp/         the ported bring-up: modeswitch, power-on, CPU enable, fwdl encoder, rxd strip
  test/        host unit tests for the ported encoders (compile + run with cc)
docs/        STATUS.md (feature matrix) · RESEARCH.md (the full bring-up recipe, cited)
```

## Quick start (the tool, on an unrooted phone)

Requires Termux + Termux:API (`pkg install termux-api`), Deno, and the adapter plugged in.

```bash
tool/ax56ctl.sh list                         # enumerate USB; note the device path
tool/ax56ctl.sh id     /dev/bus/usb/001/00X  # VID/PID/descriptor
tool/ax56ctl.sh switch /dev/bus/usb/001/00X  # if it came up as 0bda:1a2b storage → re-list, now 0b05:1997
tool/ax56ctl.sh reg    /dev/bus/usb/001/00X 00F0 1   # read R_AX_SYS_CFG1 (chip cut)
```

## Roadmap

- [x] USB transport (`wrap_sys_device`), device identification — **proven on hardware**
- [x] Storage→Wi-Fi mode switch (SCSI eject over usbfs) — **proven on hardware**
- [x] MAC register read/write (control transfers) — **proven on hardware**
- [x] Chip-cut detection (`R_AX_SYS_CFG1`) — **proven on hardware**
- [x] Power-on sequence (data-driven `mac_pwr_cfg` table) — **validated on hardware**
- [x] CPU enable/disable, firmware-download encoder (parse + txdesc + H2C) — **host-verified byte-exact**
- [ ] `dmac_pre_init` + `dle_init(DLFW)` — the packet-buffer/DMAC init the H2C path needs (in progress)
- [ ] Firmware download running on-chip
- [ ] MAC/BB/RF init + RF calibration
- [ ] Monitor (sniffer) enable, channel hopping (H2C), rxdesc → radiotap
- [ ] Raw 802.11 capture

## Credits & license

This is a **derivative work** of the GPL-2.0 Realtek vendor driver
[**lwfinger/rtl8852au**](https://github.com/lwfinger/rtl8852au) (branch `dwa-x1850`) — the source of every
register sequence, firmware layout and bring-up step ported here. Thanks also to
[morrownr/USB-WiFi](https://github.com/morrownr/USB-WiFi) for the mode-switch documentation and
[OpenIPC/devourer](https://github.com/OpenIPC/devourer) for proving the userspace approach.

Firmware images are Realtek's, distributed via
[linux-firmware `rtw89/`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtw89);
fetch them yourself (not vendored here).

Licensed **GPL-2.0** (as required by the derived driver code). See [`LICENSE`](LICENSE).
