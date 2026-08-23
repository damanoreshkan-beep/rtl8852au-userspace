cmake_minimum_required(VERSION 3.22)
project(ax56 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

# libusb as an in-tree submodule:  git submodule add https://github.com/libusb/libusb libs/libusb
set(LIBUSB ${CMAKE_SOURCE_DIR}/libs/libusb/libusb)

add_library(usb1.0 STATIC
    ${LIBUSB}/core.c ${LIBUSB}/descriptor.c ${LIBUSB}/hotplug.c ${LIBUSB}/io.c
    ${LIBUSB}/strerror.c ${LIBUSB}/sync.c
    ${LIBUSB}/os/linux_usbfs.c ${LIBUSB}/os/events_posix.c ${LIBUSB}/os/threads_posix.c
    ${LIBUSB}/os/linux_netlink.c)   # discovery is disabled at runtime; we only wrap_sys_device
target_include_directories(usb1.0 PUBLIC ${LIBUSB} ${CMAKE_SOURCE_DIR}/libs/libusb/android)
target_compile_definitions(usb1.0 PRIVATE HAVE_CONFIG_H _GNU_SOURCE)

add_library(ax56 SHARED
    cpp/native-bridge.cpp cpp/modeswitch.cpp cpp/frame-parser.cpp cpp/channel-hop.cpp
    cpp/rtw_pwron.c cpp/rtw_cpu.c cpp/rtw_dle.c cpp/rtw_fwdl.c cpp/rtw_rx.c cpp/fw_blob.c)
target_include_directories(ax56 PRIVATE ${LIBUSB})
target_link_libraries(ax56 usb1.0 android log)
# config.h: use the one from libusb/android/, do not hand-write it.
