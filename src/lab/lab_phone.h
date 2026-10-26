#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "input/control_backend.h"

namespace pm {

// What one virtual phone looks like right now, for the lab overlay. Positions are 0..1 of the phone screen.
struct LabPhoneView {
    float pointer_x = 0.5f;
    float pointer_y = 0.5f;
    bool pressed = false;
    float tap_x = 0, tap_y = 0;
    float tap_age = 99;  // seconds since the last tap
    std::wstring text;   // what has been typed
    std::wstring event;  // last phone-level event ("Home", "App Switcher", ...)
    bool connected = true;
};

// Lab stand-in for the iPhone end(s) of the Bluetooth HID link. It interprets the same reports the real
// backend sends: relative pointer motion with an iOS-like acceleration curve (clamped to the screen), taps,
// scrolling, keyboard usages -> text, consumer usages -> Home / App Switcher. Several named virtual phones
// can be "paired" at once; input goes to the targeted one exactly as the real backend routes it, and every
// event is logged as "Lab: [name] ..." for the scenario runner.
class LabPhone final : public IControlBackend {
public:
    void start(HWND notify_window) override;
    bool connected() const override { return connected_; }
    void mouse_move(int dx, int dy) override;
    void mouse_scroll(int wheel, int pan) override;
    void mouse_buttons(uint8_t buttons) override;
    void mouse_steps(int dx, int dy, int step) override;
    void keyboard(const KeyboardState& state) override;
    void consumer_click(uint16_t usage) override;
    void set_target(const std::string& phone_name) override;

    void set_connected(bool connected);
    LabPhoneView view(const std::string& phone_name);
    // In-app assertions: log "Lab: PASS ..." or "Lab: FAIL ..." for the runner.
    void expect_pointer(const std::string& phone_name, float x, float y, float tolerance);
    void expect_text(const std::string& phone_name, const std::wstring& text);

private:
    struct Phone {
        LabPhoneView view;
        uint8_t buttons = 0;
        KeyboardState keys{};
        std::chrono::steady_clock::time_point last_tap{};
        std::chrono::steady_clock::time_point last_home{};
        std::chrono::steady_clock::time_point last_log{};
    };

    Phone& current();  // caller holds mutex_
    void move(Phone& p, int dx, int dy);

    static constexpr float kWidthPoints = 402.0f;  // iPhone 17 Pro screen in points
    static constexpr float kHeightPoints = 874.0f;

    HWND notify_ = nullptr;
    std::atomic<bool> connected_ = true;
    std::mutex mutex_;
    std::map<std::string, Phone> phones_;
    std::string target_;
};

}  // namespace pm
