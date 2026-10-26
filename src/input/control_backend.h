#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace pm {

// HID boot-keyboard style state: modifier bits (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI)
// plus up to six pressed key usages (HID usage page 0x07).
struct KeyboardState {
    uint8_t modifiers = 0;
    uint8_t keys[6] = {};
    bool operator==(const KeyboardState&) const = default;
};

// Consumer-page usages (0x0C) used for phone-level actions.
inline constexpr uint16_t kConsumerHome = 0x0223;  // AC Home
inline constexpr uint16_t kConsumerVolumeUp = 0x00E9;
inline constexpr uint16_t kConsumerVolumeDown = 0x00EA;
inline constexpr uint16_t kConsumerMute = 0x00E2;
inline constexpr uint16_t kConsumerPlayPause = 0x00CD;
inline constexpr uint16_t kConsumerNext = 0x00B5;
inline constexpr uint16_t kConsumerPrevious = 0x00B6;

// Window message posted by a backend when the phone (dis)connects: wParam = 1 connected, 0 not.
inline constexpr unsigned kMsgControlState = 0x8000 + 20;  // WM_APP + 20

// A transport that makes the PC look like a keyboard + mouse to the phone. Calls are cheap and
// non-blocking from any thread; the backend coalesces and paces reports to its link.
class IControlBackend {
public:
    virtual ~IControlBackend() = default;
    virtual void start(HWND notify_window) = 0;  // posts kMsgControlState to notify_window on (dis)connect
    virtual bool connected() const = 0;
    virtual void mouse_move(int dx, int dy) = 0;
    virtual void mouse_scroll(int wheel, int pan) = 0;
    virtual void mouse_buttons(uint8_t buttons) = 0;  // bit0 left, bit1 right, bit2 middle
    // Motion sent as discrete reports of at most `step` counts per axis (never coalesced), so the phone's
    // pointer acceleration sees a constant speed: used for absolute positioning and calibration.
    virtual void mouse_steps(int dx, int dy, int step) = 0;
    virtual void keyboard(const KeyboardState& state) = 0;
    virtual void consumer_click(uint16_t usage) = 0;
    // With several phones connected, input goes only to the one with this name (its AirPlay name, matched
    // against its Bluetooth name). Empty or no match: every connected phone.
    virtual void set_target(const std::string& phone_name) = 0;
};

}  // namespace pm
