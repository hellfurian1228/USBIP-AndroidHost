#include "SyntheticEventTranslator.h"
#include "SyntheticPayloads.h"
#include "SPSCRingBuffer.h"

// Mapping from Android KeyCode to USB HID Code
// 256-element array for O(1) lookup speed in the JNI hotpath.
const uint8_t AndroidToHidMap[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, // 0-7 (KEYCODE_0 is 7)
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, // 8-15 (KEYCODE_1 to KEYCODE_8)
    0x26, 0x00, 0x00, 0x52, 0x51, 0x50, 0x4F, 0x00, // 16-23 (KEYCODE_9, DPADs)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x05, 0x06, // 24-31 (KEYCODE_A starts at 29)
    0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, // 32-39
    0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, // 40-47
    0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x00, // 48-55 (KEYCODE_Z is 54)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2B, 0x2C, 0x00, // 56-63 (Tab, Space)
    0x00, 0x00, 0x28, 0x2A, 0x35, 0x2D, 0x2E, 0x2F, // 64-71 (Enter, Del, Grave, Minus, Equals, LBracket)
    0x30, 0x31, 0x33, 0x34, 0x00, 0x00, 0x00, 0x00, // 72-79 (RBracket, Backslash, Semicolon, Apostrophe)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 80-87
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 88-95
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 96-103
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, // 104-111 (Escape is 111)
    0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 112-119 (Forward Delete is 112)
    // Remainder unmapped
};

// USB HID Modifier Bits
constexpr uint8_t MOD_LCTRL  = 0x01; // Bit 0 represents the left Control key.
constexpr uint8_t MOD_LSHIFT = 0x02; // Bit 1 represents the left Shift key.
constexpr uint8_t MOD_LALT   = 0x04; // Bit 2 represents the left Alt key.
constexpr uint8_t MOD_LGUI   = 0x08;
constexpr uint8_t MOD_RCTRL  = 0x10;
constexpr uint8_t MOD_RSHIFT = 0x20;
constexpr uint8_t MOD_RALT   = 0x40;
constexpr uint8_t MOD_RGUI   = 0x80;

// Updates the modifier byte. Returns true if it was a modifier key, false if standard.
bool HandleModifierKey(int androidKeyCode, bool isDown, uint8_t& modifierState) {
    uint8_t modBit = 0;
    switch (androidKeyCode) {
        case 113: modBit = MOD_LCTRL;  break; // KEYCODE_CTRL_LEFT
        case 114: modBit = MOD_RCTRL;  break; // KEYCODE_CTRL_RIGHT
        case 59:  modBit = MOD_LSHIFT; break; // KEYCODE_SHIFT_LEFT
        case 60:  modBit = MOD_RSHIFT; break; // KEYCODE_SHIFT_RIGHT
        case 57:  modBit = MOD_LALT;   break; // KEYCODE_ALT_LEFT
        case 58:  modBit = MOD_RALT;   break; // KEYCODE_ALT_RIGHT
        case 117: modBit = MOD_LGUI;   break; // KEYCODE_META_LEFT (Windows Key)
        case 118: modBit = MOD_RGUI;   break; // KEYCODE_META_RIGHT
        default: return false; // Not a modifier
    }

    if (isDown) {
        modifierState |= modBit;  // Set bit
    } else {
        modifierState &= ~modBit; // Clear bit
    }
    return true;
}

bool ProcessStandardKey(int androidKeyCode, bool isDown, uint8_t* keycodes) {
    uint8_t hidCode = 0x00;
    if (androidKeyCode >= 0 && androidKeyCode < 256) {
        hidCode = AndroidToHidMap[androidKeyCode];
    }

    if (hidCode == 0x00) return false;

    if (isDown) {
        bool alreadyPressed = false;
        for (int i = 0; i < 6; i++) {
            if (keycodes[i] == hidCode) {
                alreadyPressed = true;
                break;
            }
        }
        if (!alreadyPressed) {
            for (int i = 0; i < 6; i++) {
                if (keycodes[i] == 0x00) {
                    keycodes[i] = hidCode;
                    break;
                }
            }
        }
    } else {
        for (int i = 0; i < 6; i++) {
            if (keycodes[i] == hidCode) {
                keycodes[i] = 0x00;
                for (int j = i; j < 5; j++) {
                    keycodes[j] = keycodes[j + 1];
                }
                keycodes[5] = 0x00;
                break;
            }
        }
    }
    return true;
}
