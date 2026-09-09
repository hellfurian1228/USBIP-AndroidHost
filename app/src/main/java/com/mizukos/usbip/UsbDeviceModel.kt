package com.mizukos.usbip

import android.hardware.usb.UsbDevice

sealed class ExportableDevice {
    abstract val displayName: String
    abstract var isExported: Boolean

    data class Physical(
        val usbDevice: UsbDevice,
        override val displayName: String,
        override var isExported: Boolean = false,
    ) : ExportableDevice()

    data class Synthetic(
        override val displayName: String = "Virtual Keyboard & Mouse",
        override var isExported: Boolean = false,
        val isAutoFallback: Boolean = false,
    ) : ExportableDevice()
}
