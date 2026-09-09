#pragma once

#include <cstdint>

class SPSCRingBuffer; // Forward declaration

bool HandleModifierKey(int androidKeyCode, bool isDown, uint8_t& modifierState);
bool ProcessStandardKey(int androidKeyCode, bool isDown, uint8_t* keycodes);

// Mapping from Android KeyCode to USB HID Code
// O(1) lookup array — defined in SyntheticEventTranslator.cpp
extern const uint8_t AndroidToHidMap[256];
