#include <android/log.h>
#include "SyntheticUrbHandler.h"
#include "SyntheticEventTranslator.h"
#include "SyntheticUsbDevice.h"
#include "SPSCRingBuffer.h"

#define LOG_TAG "SyntheticUrbHandler"
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGW
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

extern SPSCRingBuffer* g_EventBuffer;

// Definitions for the synthetic USB descriptors declared in SyntheticUsbDescriptors.h
const UsbDeviceDescriptor G_SYNTHETIC_DEVICE_DESC;
const UsbConfigDescriptorBundle G_SYNTHETIC_CONFIG_BUNDLE;

PendingInterruptURB g_PendingHidUrb{};
static SyntheticKeyboardReport g_CurrentKeyboardState{0x02, 0, 0, {0, 0, 0, 0, 0, 0}};

void ResetSyntheticState() {
    g_PendingHidUrb.isActive.store(false, std::memory_order_release);
    g_PendingHidUrb.seqnum = 0;
    g_CurrentKeyboardState = {0x02, 0, 0, {0, 0, 0, 0, 0, 0}};
}

SyntheticUrbResponse HandleSyntheticSubmit(uint32_t seqnum, uint32_t ep, uint32_t direction, const uint8_t* setupPacket) {
    SyntheticUrbResponse resp{};
    resp.seqnum = seqnum;
    resp.ep = ep;

    if (ep == 0) {
        resp.hasData = true; // MUST be true so usbip_server sends RET_SUBMIT packet to Windows
        uint8_t bmRequestType = setupPacket[0];
        uint8_t bRequest      = setupPacket[1];
        uint16_t wValue       = (setupPacket[3] << 8) | setupPacket[2];
        uint16_t wIndex       = (setupPacket[5] << 8) | setupPacket[4];
        uint16_t wLength      = (setupPacket[7] << 8) | setupPacket[6];

        LOGI("Synthetic Submit EP:%d, bmReq:0x%02x, bReq:0x%02x, wVal:0x%04x, wIdx:0x%04x, wLen:%d",
             ep, bmRequestType, bRequest, wValue, wIndex, wLength);

        // 1. GET_DESCRIPTOR (Device = 0x80, Interface/HID Report = 0x81)
        if ((bmRequestType == 0x80 || bmRequestType == 0x81) && bRequest == 0x06) {
            uint8_t descType = (wValue >> 8) & 0xFF;
            uint8_t descIndex = wValue & 0xFF;

            const char* descTypeName = "UNKNOWN";
            if (descType == 0x01) descTypeName = "DEVICE (0x01)";
            else if (descType == 0x02) descTypeName = "CONFIGURATION (0x02)";
            else if (descType == 0x03) descTypeName = "STRING (0x03)";
            else if (descType == 0x21) descTypeName = "HID_HEADER (0x21)";
            else if (descType == 0x22) descTypeName = "HID_REPORT (0x22)";

            LOGI("Synthetic GET_DESCRIPTOR req [%s] descIdx:0x%02x, wIndex:0x%04x, requested wLength:%d",
                 descTypeName, descIndex, wIndex, wLength);

            int len = SyntheticUsbDevice::HandleControlGetDescriptor(descType, descIndex, resp.payload, wLength);

            if (len >= 0) {
                LOGI("Synthetic GET_DESCRIPTOR [%s] SUCCESS -> returned len:%d bytes (req wLen:%d)",
                     descTypeName, len, wLength);
                resp.payloadLen = len;
                resp.status = 0;
                return resp;
            } else {
                LOGW("Synthetic GET_DESCRIPTOR [%s] UNHANDLED/FAILED -> STALL", descTypeName);
            }
        }

        // 2. SET_CONFIGURATION (bRequest 0x09)
        if ((bmRequestType == 0x00) && bRequest == 0x09) {
            LOGI("Synthetic SET_CONFIGURATION (val:0x%04x)", wValue);
            resp.payloadLen = 0;
            resp.status = 0;
            return resp;
        }

        // 3. SET_ADDRESS (bRequest 0x05)
        if ((bmRequestType == 0x00) && bRequest == 0x05) {
            LOGI("Synthetic SET_ADDRESS (addr:0x%04x)", wValue);
            resp.payloadLen = 0;
            resp.status = 0;
            return resp;
        }

        // 4. GET_STATUS (bRequest 0x00)
        if ((bmRequestType & 0x80) && bRequest == 0x00) {
            LOGI("Synthetic GET_STATUS");
            resp.payload[0] = 0x00;
            resp.payload[1] = 0x00;
            resp.payloadLen = (wLength >= 2) ? 2 : wLength;
            resp.status = 0;
            return resp;
        }

        // 5. HID Class Requests: SET_IDLE (0x0A), SET_PROTOCOL (0x0B)
        if (bmRequestType == 0x21 && (bRequest == 0x0A || bRequest == 0x0B)) {
            LOGI("Synthetic HID Class Req: 0x%02x", bRequest);
            resp.payloadLen = 0;
            resp.status = 0;
            return resp;
        }

        // Unhandled request -> STALL
        LOGE("Synthetic Submit UNHANDLED bmReq:0x%02x, bReq:0x%02x, wVal:0x%04x, wIdx:0x%04x, wLen:%d -> STALL",
             bmRequestType, bRequest, wValue, wIndex, wLength);
        resp.payloadLen = 0;
        resp.status = 1;
        return resp;
    }

    if (ep == 1 && direction == 1) {
        g_PendingHidUrb.seqnum = seqnum;
        g_PendingHidUrb.isActive.store(true, std::memory_order_release);
        resp.hasData = false; // Parked until PollSyntheticEvents produces input
        return resp;
    }

    return resp;
}

SyntheticUrbResponse PollSyntheticEvents() {
    SyntheticUrbResponse resp{};
    if (!g_PendingHidUrb.isActive.load(std::memory_order_acquire)) return resp;

    SyntheticEvent ev{};

    if (g_EventBuffer && g_EventBuffer->pop(ev)) {
        g_PendingHidUrb.isActive.store(false, std::memory_order_release);
        resp.hasData = true;
        resp.seqnum = g_PendingHidUrb.seqnum;
        resp.ep = 1;
        resp.status = 0;

        if (ev.type == SyntheticEventType::MOUSE) {
            SyntheticMouseReport mousePayload{};
            mousePayload.reportId = 0x01;
            mousePayload.buttons = static_cast<uint8_t>(ev.buttons & 0x1F);
            mousePayload.dx = static_cast<int16_t>(ev.dx);
            mousePayload.dy = static_cast<int16_t>(ev.dy);
            mousePayload.scroll = static_cast<int8_t>(ev.scroll);

            std::memcpy(resp.payload, &mousePayload, sizeof(mousePayload));
            resp.payloadLen = sizeof(mousePayload);

        } else if (ev.type == SyntheticEventType::KEYBOARD) {
            if (HandleModifierKey(ev.keyCode, ev.isDown, g_CurrentKeyboardState.modifiers) ||
                ProcessStandardKey(ev.keyCode, ev.isDown, g_CurrentKeyboardState.keycodes)) {
                std::memcpy(resp.payload, &g_CurrentKeyboardState, sizeof(g_CurrentKeyboardState));
                resp.payloadLen = sizeof(g_CurrentKeyboardState);
            } else {
                g_PendingHidUrb.isActive.store(true, std::memory_order_release);
                resp.hasData = false;
            }
        }
    }
    return resp;
}
