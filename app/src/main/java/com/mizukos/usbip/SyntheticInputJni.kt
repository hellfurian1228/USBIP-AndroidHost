package com.mizukos.usbip

import android.view.MotionEvent
import android.view.View

object SyntheticInputJni {
    init {
        System.loadLibrary("usbip_server")
    }

    @JvmStatic
    external fun initRingBuffer(capacity: Int)
    
    @JvmStatic
    external fun destroyRingBuffer()
    
    @JvmStatic
    external fun pushMouseEvent(buttons: Int, dx: Float, dy: Float, scroll: Float)
    
    @JvmStatic
    external fun pushKeyboardEvent(keyCode: Int, isDown: Boolean)
    
    @JvmStatic
    external fun clearAllStates()

    fun attachToView(view: View) {
        view.isFocusable = true
        view.isFocusableInTouchMode = true
        view.requestFocus()

        // Capture raw relative mouse movements & clicks (unaffected by UI focus)
        view.setOnCapturedPointerListener { _, event ->
            val dx = event.getAxisValue(MotionEvent.AXIS_RELATIVE_X)
            val dy = event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y)
            val scroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
            val buttons = event.buttonState 
            
            pushMouseEvent(buttons, dx, dy, scroll)
            true // Consume event
        }

        // Locks the mouse to the app and hides the Android system cursor
        view.requestPointerCapture()
    }
}
