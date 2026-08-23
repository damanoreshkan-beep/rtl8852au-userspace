package world.anubis.ax56

import android.hardware.usb.*

/**
 * Bridges the Android USB fd into the native core. Every native entry point maps
 * to a proven or roadmapped primitive in native/cpp (see docs/STATUS.md).
 */
class UsbController(
    private val usb: UsbManager,
    private val log: (String) -> Unit,
) {
    private var conn: UsbDeviceConnection? = null
    private var handle: Long = 0

    companion object { init { System.loadLibrary("ax56") } }

    // --- native bridge ---
    external fun nativeModeSwitch(fd: Int): Int          // ✅ SCSI eject
    external fun nativeInitDriver(fd: Int): Long          // ⛔ fw download WIP → 0 until ported
    external fun nativeStartScan(handle: Long): Int       // ⛔ needs RX
    external fun nativeStopScan(handle: Long)
    external fun nativeInjectRaw(handle: Long, frame: ByteArray): Int  // 🟡 generic bulk-OUT TX
    external fun nativeClose(handle: Long)

    fun modeSwitch(dev: UsbDevice) {
        val fd = open(dev) ?: return
        log(if (nativeModeSwitch(fd) == 0) "eject ok — waiting for re-enumerate" else "eject failed")
        close()   // fd dies on detach; the new device arrives as ATTACH
    }

    fun startWifi(dev: UsbDevice) {
        val fd = open(dev) ?: return
        handle = nativeInitDriver(fd)
        if (handle == 0L) { log("initDriver: firmware/monitor port not implemented yet (see STATUS.md)"); return }
        nativeStartScan(handle)
    }

    private fun open(dev: UsbDevice): Int? {
        val c = usb.openDevice(dev) ?: run { log("openDevice returned null"); return null }
        conn = c
        return c.fileDescriptor   // → libusb_wrap_sys_device on the native side
    }

    fun close() { if (handle != 0L) { nativeClose(handle); handle = 0 }; conn?.close(); conn = null }
}
