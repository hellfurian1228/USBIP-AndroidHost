package com.mizukos.usbip

import android.content.Context
import android.hardware.input.InputManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbManager
import android.view.InputDevice
import android.view.View

class SyntheticFallbackManager(context: Context, private val logger: ErrorLogger = ErrorLogger) {
    private val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager

    fun evaluateSilentFallback(view: View): Boolean {
        var hasPhysicalHid = false
        
        // 1. Check if Android OS framework detects a mouse or keyboard
        for (id in inputManager.inputDeviceIds) {
            val dev = inputManager.getInputDevice(id)
            if (dev != null && !dev.isVirtual) {
                if (dev.sources and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE ||
                    dev.sources and InputDevice.SOURCE_KEYBOARD == InputDevice.SOURCE_KEYBOARD) {
                    hasPhysicalHid = true
                    break
                }
            }
        }

        // 2. Check if the raw UsbManager has access to a HID interface
        val rawHidExists = usbManager.deviceList.values.any { dev ->
            (0 until dev.interfaceCount).any { i ->
                dev.getInterface(i).interfaceClass == UsbConstants.USB_CLASS_HID
            }
        }

        // 3. If OS sees it, but UsbManager is blocked from accessing it
        if (hasPhysicalHid && !rawHidExists) {
            logger.info("OS intercept detected. Silent fallback to Synthetic Input triggered.")
            attachSyntheticEngine(view)
            return true
        }
        
        return false
    }

    fun attachSyntheticEngine(view: View) {
        SyntheticInputJni.attachToView(view)
        logger.info("Synthetic HID Engine attached to View.")
        
        view.onFocusChangeListener = View.OnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) {
                logger.warn("Focus lost. Clearing synthetic states.")
                SyntheticInputJni.clearAllStates()
            }
        }

        view.addOnAttachStateChangeListener(
            object : View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(v: View) {}
                override fun onViewDetachedFromWindow(v: View) {
                    logger.warn("View detached from window. Clearing synthetic states.")
                    SyntheticInputJni.clearAllStates()
                }
            }
        )
    }

    fun detachSyntheticEngine(view: View) {
        view.onFocusChangeListener = null
        SyntheticInputJni.clearAllStates()
        logger.info("Synthetic HID Engine detached.")
    }
}
