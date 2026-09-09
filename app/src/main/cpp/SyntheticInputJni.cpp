#include <jni.h>
#include "SPSCRingBuffer.h"

// Global pointer for your network worker thread to access
SPSCRingBuffer* g_EventBuffer = nullptr;

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_SyntheticInputJni_initRingBuffer(JNIEnv* env, jclass clazz, jint capacity) {
    if (!g_EventBuffer) {
        g_EventBuffer = new SPSCRingBuffer(capacity);
    }
}

// Cleanup function to prevent native memory leaks during Android Service restarts.
extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_SyntheticInputJni_destroyRingBuffer(JNIEnv* env, jclass clazz) {
    if (g_EventBuffer) {
        delete g_EventBuffer;
        g_EventBuffer = nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_SyntheticInputJni_pushMouseEvent(JNIEnv* env, jclass clazz, jint buttons, jfloat dx, jfloat dy, jfloat scroll) {
    if (g_EventBuffer) {
        SyntheticEvent ev{};
        ev.type = SyntheticEventType::MOUSE;
        ev.buttons = buttons;
        ev.dx = dx;
        ev.dy = dy;
        ev.scroll = scroll;
        g_EventBuffer->push(ev);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_SyntheticInputJni_pushKeyboardEvent(JNIEnv* env, jclass clazz, jint keyCode, jboolean isDown) {
    if (g_EventBuffer) {
        SyntheticEvent ev{};
        ev.type = SyntheticEventType::KEYBOARD;
        ev.keyCode = keyCode;
        ev.isDown = isDown;
        g_EventBuffer->push(ev);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_SyntheticInputJni_clearAllStates(JNIEnv* env, jclass clazz) {
    if (g_EventBuffer) {
        // Send a zeroed mouse event
        SyntheticEvent mouseEv{};
        mouseEv.type = SyntheticEventType::MOUSE;
        g_EventBuffer->push(mouseEv); // All fields default to 0

        // Send a zeroed keyboard event
        SyntheticEvent kbEv{};
        kbEv.type = SyntheticEventType::KEYBOARD;
        kbEv.keyCode = 0; // Handled as empty in translator
        kbEv.isDown = false;

        g_EventBuffer->push(kbEv);
    }
}
