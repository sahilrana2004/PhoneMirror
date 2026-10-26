#pragma once

#include <cstdint>

namespace pm {

// Maps a physical key (PS/2 set-1 scan code + extended flag, as delivered by WH_KEYBOARD_LL) to a HID
// keyboard usage. Physical positions are sent, so the phone applies its own keyboard layout.
// Modifier keys return 0 and set *modifier_bit instead. Unmapped keys return 0 with no bit.
uint8_t hid_usage_for_scan(uint32_t scan, bool extended, uint8_t* modifier_bit);

// Media keys (by virtual-key code) to consumer-page usages; 0 if not a media key.
uint16_t consumer_usage_for_vk(uint32_t vk);

}  // namespace pm
