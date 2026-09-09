#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include <arpa/inet.h>
#include <vector>
#include "SyntheticUsbDescriptors.h"
#include "SyntheticPayloads.h"

struct SyntheticUrbResponse {
    bool hasData = false;
    uint32_t seqnum = 0;
    uint32_t ep = 0;
    int status = 0;
    uint8_t payload[1024] = {0};
    uint32_t payloadLen = 0;
};

// Parked URB state for the asynchronous Interrupt IN endpoint
struct PendingInterruptURB {
    std::atomic<bool> isActive{false};
    uint32_t seqnum = 0;
};

extern PendingInterruptURB g_PendingHidUrb;

SyntheticUrbResponse HandleSyntheticSubmit(uint32_t seqnum, uint32_t ep, uint32_t direction, const uint8_t* setupPacket);
SyntheticUrbResponse PollSyntheticEvents();
void ResetSyntheticState();

// USB/IP protocol strict device export format
#pragma pack(push, 1)
struct UsbIpDeviceExportInfo {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;

    // Interface 0
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
};
#pragma pack(pop)

// Call this inside your OP_REP_DEVLIST handler after packing physical devices
inline void AppendSyntheticDeviceToExportList(std::vector<uint8_t>& responseBuffer) {
    UsbIpDeviceExportInfo info{};

    // String fields (null-terminated)
    std::strncpy(info.path, "/virtual/synthetic_hid", sizeof(info.path) - 1);
    std::strncpy(info.busid, "synthetic-1", sizeof(info.busid) - 1);

    // Network Byte Order conversions (htonl for 32-bit, htons for 16-bit)
    info.busnum = htonl(99);
    info.devnum = htonl(1);
    info.speed = htonl(2); // Full Speed (12 Mbps)

    info.idVendor = htons(0x1209);
    info.idProduct = htons(0x0001);
    info.bcdDevice = htons(0x0100);

    // Device level
    info.bDeviceClass = 0x00;
    info.bDeviceSubClass = 0x00;
    info.bDeviceProtocol = 0x00;
    info.bConfigurationValue = 1;
    info.bNumConfigurations = 1;
    info.bNumInterfaces = 1;

    // Interface level (HID = 0x03)
    info.bInterfaceClass = 0x03;
    info.bInterfaceSubClass = 0x00;
    info.bInterfaceProtocol = 0x00;
    info.padding = 0x00;

    // Append to your active TCP buffer
    uint8_t* rawData = reinterpret_cast<uint8_t*>(&info);
    responseBuffer.insert(responseBuffer.end(), rawData, rawData + sizeof(UsbIpDeviceExportInfo));
}
