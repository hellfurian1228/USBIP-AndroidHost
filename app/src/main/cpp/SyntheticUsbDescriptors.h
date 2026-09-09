#pragma once
#include <cstdint>
#include "SyntheticDescriptors.h"

#pragma pack(push, 1)

// ==========================================
// 1. USB Standard Device Descriptor (18 Bytes)
// ==========================================
struct UsbDeviceDescriptor {
    uint8_t  bLength            = 18;
    uint8_t  bDescriptorType    = 0x01; // DEVICE
    uint16_t bcdUSB             = 0x0110; // USB 1.10 (Standard for Full-Speed HID)
    uint8_t  bDeviceClass       = 0x00; // Specified at Interface level
    uint8_t  bDeviceSubClass    = 0x00;
    uint8_t  bDeviceProtocol    = 0x00;
    uint8_t  bMaxPacketSize0    = 64;
    uint16_t idVendor           = 0x1209; // Generic / Open Source VID
    uint16_t idProduct          = 0x0001; // Synthetic HID PID
    uint16_t bcdDevice          = 0x0100; // Device Rev 1.00
    uint8_t  iManufacturer      = 0;
    uint8_t  iProduct           = 0;
    uint8_t  iSerialNumber      = 0;
    uint8_t  bNumConfigurations = 1;
};

// ==========================================
// 2. Full Configuration Descriptor Bundle
// ==========================================
struct UsbConfigDescriptorBundle {
    // Configuration Descriptor (9 Bytes)
    struct {
        uint8_t  bLength             = 9;
        uint8_t  bDescriptorType     = 0x02; // CONFIGURATION
        uint16_t wTotalLength        = sizeof(UsbConfigDescriptorBundle);
        uint8_t  bNumInterfaces      = 1;
        uint8_t  bConfigurationValue = 1;
        uint8_t  iConfiguration      = 0;
        uint8_t  bmAttributes        = 0xA0; // Bus-Powered, Remote Wakeup
        uint8_t  bMaxPower           = 50;   // 100mA
    } config;

    // Interface Descriptor (9 Bytes)
    struct {
        uint8_t  bLength            = 9;
        uint8_t  bDescriptorType    = 0x04; // INTERFACE
        uint8_t  bInterfaceNumber   = 0;
        uint8_t  bAlternateSetting  = 0;
        uint8_t  bNumEndpoints      = 1;    // 1 Interrupt IN endpoint
        uint8_t  bInterfaceClass    = 0x03; // HID Class
        uint8_t  bInterfaceSubClass = 0x00; // Non-boot (handled by report descriptor)
        uint8_t  bInterfaceProtocol = 0x00;
        uint8_t  iInterface         = 0;
    } interface;

    // HID Descriptor (9 Bytes)
    struct {
        uint8_t  bLength           = 9;
        uint8_t  bDescriptorType   = 0x21; // HID
        uint16_t bcdHID            = 0x0111; // HID v1.11
        uint8_t  bCountryCode      = 0x00;
        uint8_t  bNumDescriptors   = 1;
        uint8_t  bReportDescriptorType = 0x22; // REPORT
        uint16_t wDescriptorLength = sizeof(SYNTHETIC_HID_REPORT_DESCRIPTOR);
    } hid;

    // Endpoint Descriptor: EP1 IN (7 Bytes)
    struct {
        uint8_t  bLength          = 7;
        uint8_t  bDescriptorType  = 0x05; // ENDPOINT
        uint8_t  bEndpointAddress = 0x81; // EP 1 IN
        uint8_t  bmAttributes     = 0x03; // Transfer Type: Interrupt
        uint16_t wMaxPacketSize   = 64;   // 64 Bytes packet size
        uint8_t  bInterval        = 1;    // 1ms polling interval for ultra-low latency
    } endpoint;
};

#pragma pack(pop)

extern const UsbDeviceDescriptor G_SYNTHETIC_DEVICE_DESC;
extern const UsbConfigDescriptorBundle G_SYNTHETIC_CONFIG_BUNDLE;
