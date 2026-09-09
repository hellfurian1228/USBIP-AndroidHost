#pragma once
#include <cstring>
#include "SyntheticUsbDescriptors.h"

struct SyntheticUsbDevice {
    const char* busid = "synthetic-1";
    const char* path  = "/virtual/synthetic_hid";
    uint32_t busnum   = 99;
    uint32_t devnum   = 1;
    uint32_t speed    = 2; // USB_SPEED_FULL (12 Mbps)

    // Handle standard USB GET_DESCRIPTOR requests on Endpoint 0
    static int HandleControlGetDescriptor(uint8_t descType, uint8_t descIndex, uint8_t* outBuffer, uint16_t maxLength) {
        const uint8_t* src = nullptr;
        uint16_t len = 0;

        switch (descType) {
            case 0x01: // Device Descriptor
                src = reinterpret_cast<const uint8_t*>(&G_SYNTHETIC_DEVICE_DESC);
                len = sizeof(G_SYNTHETIC_DEVICE_DESC);
                break;

            case 0x02: // Configuration Descriptor
                src = reinterpret_cast<const uint8_t*>(&G_SYNTHETIC_CONFIG_BUNDLE);
                len = sizeof(G_SYNTHETIC_CONFIG_BUNDLE);
                break;

            case 0x22: // HID Report Descriptor
                src = SYNTHETIC_HID_REPORT_DESCRIPTOR;
                len = sizeof(SYNTHETIC_HID_REPORT_DESCRIPTOR);
                break;

            default:
                return -1; // Unhandled descriptor type (STALL)
        }

        uint16_t copyBytes = (len < maxLength) ? len : maxLength;
        std::memcpy(outBuffer, src, copyBytes);
        return copyBytes;
    }
};
