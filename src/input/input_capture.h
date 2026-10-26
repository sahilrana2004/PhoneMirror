#pragma once

#include <windows.h>

#include <functional>

#include "input/control_backend.h"

namespace pm {

// Routes the PC's mouse and keyboard to the phone while the mirror window is foreground and captured.
// Capture starts with a click in the window and ends on Ctrl+Alt, focus loss, or phone disconnect.
// Mouse: Raw Input deltas (unaccelerated) -> HID relative motion; the Windows cursor is hidden and clipped.
// Keyboard: a low-level hook (installed only while captured) so Win/Alt+Tab/etc. reach the phone too.
class InputCapture {
public:
    InputCapture() = default;
    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;
    ~InputCapture() { end(); }

    void attach(HWND hwnd, IControlBackend* backend) {
        hwnd_ = hwnd;
        backend_ = backend;
    }
    bool active() const { return active_; }
    bool begin();
    void end();

    // Called from the window procedure; returns true when the message was consumed.
    bool handle(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);

    // Notified (on the window thread) when capture starts or stops, for on-screen hints.
    std::function<void(bool)> on_state;

private:
    static LRESULT CALLBACK keyboard_hook(int code, WPARAM wp, LPARAM lp);
    bool on_key(const KBDLLHOOKSTRUCT& k, bool down);
    void on_raw_input(HRAWINPUT input);
    void send_keyboard();
    void release_everything();

    HWND hwnd_ = nullptr;
    IControlBackend* backend_ = nullptr;
    bool active_ = false;
    HHOOK hook_ = nullptr;

    KeyboardState keyboard_{};
    uint8_t physical_mods_ = 0;  // what the user is holding (unswapped), for local shortcuts
    uint8_t buttons_ = 0;
    double carry_x_ = 0;
    double carry_y_ = 0;
    int wheel_carry_ = 0;
    int pan_carry_ = 0;
    LONG last_abs_x_ = -1;
    LONG last_abs_y_ = -1;

    // Settings (fixed until the settings UI lands).
    bool swap_ctrl_cmd_ = true;  // Ctrl+C/V/Z behave as on Windows (Ctrl -> Command, Win -> Control)
    double pointer_gain_ = 1.0;
};

inline constexpr UINT kMsgReleaseCapture = WM_APP + 21;

}  // namespace pm
