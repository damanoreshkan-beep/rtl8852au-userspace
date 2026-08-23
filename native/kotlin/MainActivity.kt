package world.anubis.ax56

import android.app.PendingIntent
import android.content.*
import android.hardware.usb.*
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

private const val ACTION_USB_PERMISSION = "world.anubis.ax56.USB_PERMISSION"
private const val VID_REALTEK = 0x0bda; private const val PID_STORAGE = 0x1a2b
private const val VID_ASUS    = 0x0b05; private const val PID_WIFI    = 0x1997

class MainActivity : AppCompatActivity() {
    private lateinit var usb: UsbManager
    private lateinit var controller: UsbController
    private lateinit var log: TextView

    private val permReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val dev: UsbDevice = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE) ?: return
            if (!intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                logln("USB permission denied for ${dev.productName}"); return
            }
            onDeviceReady(dev)
        }
    }
    // After modeswitch the device re-enumerates → catch the new ATTACH.
    private val attachReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            val dev: UsbDevice = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE) ?: return
            requestPermission(dev)
        }
    }

    override fun onCreate(s: Bundle?) {
        super.onCreate(s)
        log = TextView(this).also { setContentView(it) }
        usb = getSystemService(Context.USB_SERVICE) as UsbManager
        controller = UsbController(usb) { line -> runOnUiThread { logln(line) } }
        registerReceiver(permReceiver, IntentFilter(ACTION_USB_PERMISSION), RECEIVER_NOT_EXPORTED)
        registerReceiver(attachReceiver, IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED))
        usb.deviceList.values.firstOrNull { matches(it) }?.let { requestPermission(it) }
    }

    private fun matches(d: UsbDevice) =
        (d.vendorId == VID_REALTEK && d.productId == PID_STORAGE) ||
        (d.vendorId == VID_ASUS && d.productId == PID_WIFI)

    private fun requestPermission(dev: UsbDevice) {
        if (!matches(dev)) return
        val pi = PendingIntent.getBroadcast(this, 0,
            Intent(ACTION_USB_PERMISSION).setPackage(packageName), PendingIntent.FLAG_IMMUTABLE)
        usb.requestPermission(dev, pi)
    }

    private fun onDeviceReady(dev: UsbDevice) = when {
        dev.vendorId == VID_REALTEK && dev.productId == PID_STORAGE -> {
            logln("Storage mode — sending eject…"); controller.modeSwitch(dev)
        }
        dev.vendorId == VID_ASUS && dev.productId == PID_WIFI -> {
            logln("Wi-Fi mode — initialising driver"); controller.startWifi(dev)
        }
        else -> Unit
    }

    private fun logln(s: String) { log.append("$s\n") }
    override fun onDestroy() {
        unregisterReceiver(permReceiver); unregisterReceiver(attachReceiver)
        controller.close(); super.onDestroy()
    }
}
