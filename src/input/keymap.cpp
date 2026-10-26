#include "input/keymap.h"

#include <windows.h>

#include "input/control_backend.h"

namespace pm {
namespace {

// Index: set-1 scan code (non-extended). Value: HID usage (page 0x07).
constexpr uint8_t kBase[0x59] = {
    0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,  // 00-0F
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0,    0x04, 0x16,  // 10-1F
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0,    0x31, 0x1D, 0x1B, 0x06, 0x19,  // 20-2F
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0,    0x55, 0,    0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,  // 30-3F
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,  // 40-4F
    0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44, 0x45,                                            // 50-58
};

}  // namespace

uint8_t hid_usage_for_scan(uint32_t scan, bool extended, uint8_t* modifier_bit) {
    *modifier_bit = 0;
    if (!extended) {
        switch (scan) {
        case 0x1D: *modifier_bit = 0x01; return 0;  // LCtrl
        case 0x2A: *modifier_bit = 0x02; return 0;  // LShift
        case 0x38: *modifier_bit = 0x04; return 0;  // LAlt
        case 0x36: *modifier_bit = 0x20; return 0;  // RShift
        }
        return scan < sizeof(kBase) ? kBase[scan] : 0;
    }
    switch (scan) {
    case 0x1D: *modifier_bit = 0x10; return 0;  // RCtrl
    case 0x38: *modifier_bit = 0x40; return 0;  // RAlt
    case 0x5B: *modifier_bit = 0x08; return 0;  // LWin
    case 0x5C: *modifier_bit = 0x80; return 0;  // RWin
    case 0x1C: return 0x58;  // keypad Enter
    case 0x35: return 0x54;  // keypad /
    case 0x37: return 0x46;  // Print Screen
    case 0x45: return 0x53;  // Num Lock (reported extended by the LL hook)
    case 0x47: return 0x4A;  // Home
    case 0x48: return 0x52;  // Up
    case 0x49: return 0x4B;  // Page Up
    case 0x4B: return 0x50;  // Left
    case 0x4D: return 0x4F;  // Right
    case 0x4F: return 0x4D;  // End
    case 0x50: return 0x51;  // Down
    case 0x51: return 0x4E;  // Page Down
    case 0x52: return 0x49;  // Insert
    case 0x53: return 0x4C;  // Delete
    case 0x5D: return 0x65;  // Application / menu
    }
    return 0;
}

uint16_t consumer_usage_for_vk(uint32_t vk) {
    switch (vk) {
    case VK_VOLUME_UP: return kConsumerVolumeUp;
    case VK_VOLUME_DOWN: return kConsumerVolumeDown;
    case VK_VOLUME_MUTE: return kConsumerMute;
    case VK_MEDIA_PLAY_PAUSE: return kConsumerPlayPause;
    case VK_MEDIA_NEXT_TRACK: return kConsumerNext;
    case VK_MEDIA_PREV_TRACK: return kConsumerPrevious;
    }
    return 0;
}

}  // namespace pm
