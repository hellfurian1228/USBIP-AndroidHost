#pragma once
#include <cstdint>

#pragma pack(push, 1) // Force 1-byte alignment

// Total Size: 7 Bytes
struct SyntheticMouseReport {
    uint8_t reportId; // Always 0x01
    uint8_t buttons;  // Bit 0:Left, 1:Right, 2:Middle, 3:Back, 4:Forward
    int16_t dx;       // Signed 16-bit X delta
    int16_t dy;       // Signed 16-bit Y delta
    int8_t scroll;    // Signed 8-bit vertical wheel delta
};

// Total Size: 9 Bytes
struct SyntheticKeyboardReport {
    uint8_t reportId;      // Always 0x02
    uint8_t modifiers;     // Bitfield (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI)
    uint8_t reserved;      // Always 0x00
    uint8_t keycodes[6];   // Active USB HID Usage IDs
};

#pragma pack(pop)
