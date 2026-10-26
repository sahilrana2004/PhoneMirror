#include "lab/lab_phone.h"

#include <algorithm>
#include <cmath>

#include "app/log.h"

namespace pm {
namespace {

using Clock = std::chrono::steady_clock;

// HID usage (page 0x07) -> character, unshifted / shifted (US layout, enough for scenarios).
wchar_t usage_char(uint8_t usage, bool shift) {
    if (usage >= 0x04 && usage <= 0x1D) return static_cast<wchar_t>((shift ? L'A' : L'a') + (usage - 0x04));
    if (usage >= 0x1E && usage <= 0x26) return shift ? L"!@#$%^&*("[usage - 0x1E] : static_cast<wchar_t>(L'1' + (usage - 0x1E));
    switch (usage) {
    case 0x27: return shift ? L')' : L'0';
    case 0x2C: return L' ';
    case 0x2D: return shift ? L'_' : L'-';
    case 0x2E: return shift ? L'+' : L'=';
    case 0x33: return shift ? L':' : L';';
    case 0x34: return shift ? L'"' : L'\'';
    case 0x36: return shift ? L'<' : L',';
    case 0x37: return shift ? L'>' : L'.';
    case 0x38: return shift ? L'?' : L'/';
    }
    return 0;
}

}  // namespace

void LabPhone::start(HWND notify_window) {
    notify_ = notify_window;
    log::info("Lab: virtual iPhone ready (keyboard + mouse connected)");
    PostMessageW(notify_, kMsgControlState, 1, 0);
}

void LabPhone::set_connected(bool connected) {
    if (connected_.exchange(connected) == connected) return;
    log::info("Lab: virtual iPhone {}", connected ? "connected" : "disconnected");
    PostMessageW(notify_, kMsgControlState, connected ? 1 : 0, 0);
}

void LabPhone::set_target(const std::string& phone_name) {
    std::lock_guard lock(mutex_);
    if (phone_name == target_) return;
    target_ = phone_name;
    log::info("Lab: input now goes to [{}]", phone_name);
}

LabPhone::Phone& LabPhone::current() { return phones_[target_]; }

// AssistiveTouch-like response: points per count grow with report speed (acceleration), position clamps
// at the screen edges - which is exactly what makes "slam to the corner" work as a position reset.
void LabPhone::move(Phone& p, int dx, int dy) {
    float speed = static_cast<float>(std::max(std::abs(dx), std::abs(dy)));
    float gain = 0.5f * (1.0f + 0.04f * std::min(speed, 100.0f));
    p.view.pointer_x = std::clamp(p.view.pointer_x + dx * gain / kWidthPoints, 0.0f, 1.0f);
    p.view.pointer_y = std::clamp(p.view.pointer_y + dy * gain / kHeightPoints, 0.0f, 1.0f);
    auto now = Clock::now();
    if (now - p.last_log > std::chrono::milliseconds(250)) {
        p.last_log = now;
        log::info("Lab: [{}] pointer at ({:.3f}, {:.3f})", target_, p.view.pointer_x, p.view.pointer_y);
    }
}

void LabPhone::mouse_move(int dx, int dy) {
    if (!connected_) return;
    std::lock_guard lock(mutex_);
    move(current(), dx, dy);
}

void LabPhone::mouse_steps(int dx, int dy, int step) {
    if (!connected_) return;
    std::lock_guard lock(mutex_);
    Phone& p = current();
    step = std::max(step, 1);
    while (dx || dy) {
        int sx = std::clamp(dx, -step, step);
        int sy = std::clamp(dy, -step, step);
        move(p, sx, sy);
        dx -= sx;
        dy -= sy;
    }
    log::info("Lab: [{}] pointer at ({:.3f}, {:.3f})", target_, p.view.pointer_x, p.view.pointer_y);
}

void LabPhone::mouse_scroll(int wheel, int pan) {
    if (!connected_) return;
    std::lock_guard lock(mutex_);
    log::info("Lab: [{}] scroll {} / pan {}", target_, wheel, pan);
    current().view.event = std::format(L"Scroll {}", wheel ? wheel : pan);
}

void LabPhone::mouse_buttons(uint8_t buttons) {
    if (!connected_) return;
    std::lock_guard lock(mutex_);
    Phone& p = current();
    bool was_down = p.buttons & 0x01;
    bool down = buttons & 0x01;
    p.buttons = buttons;
    p.view.pressed = down;
    if (was_down && !down) {
        p.view.tap_x = p.view.pointer_x;
        p.view.tap_y = p.view.pointer_y;
        p.last_tap = Clock::now();
        log::info("Lab: [{}] tap at ({:.3f}, {:.3f})", target_, p.view.pointer_x, p.view.pointer_y);
    }
    if (buttons & 0x02) {
        p.view.event = L"Secondary click";
        log::info("Lab: [{}] secondary click", target_);
    }
}

void LabPhone::keyboard(const KeyboardState& state) {
    if (!connected_) return;
    std::lock_guard lock(mutex_);
    Phone& p = current();
    bool shift = state.modifiers & 0x22;
    bool command = state.modifiers & 0x88;
    for (uint8_t usage : state.keys) {
        if (!usage) continue;
        if (std::find(std::begin(p.keys.keys), std::end(p.keys.keys), usage) != std::end(p.keys.keys)) continue;
        if (command && usage == 0x2C) {
            p.view.event = L"Spotlight";
            log::info("Lab: [{}] Spotlight (Command+Space)", target_);
        } else if (command) {
            wchar_t c = usage_char(usage, false);
            p.view.event = std::wstring(L"Command+") + (c ? std::wstring(1, static_cast<wchar_t>(towupper(c))) : L"?");
            log::info("Lab: [{}] shortcut Command+{}", target_, log::utf8(std::wstring(1, c ? c : L'?')));
        } else if (usage == 0x2A) {  // Backspace
            if (!p.view.text.empty()) p.view.text.pop_back();
        } else if (usage == 0x28) {  // Enter
            log::info("Lab: [{}] text '{}'", target_, log::utf8(p.view.text));
            p.view.event = L"Return";
        } else if (wchar_t c = usage_char(usage, shift)) {
            p.view.text.push_back(c);
        }
    }
    p.keys = state;
}

void LabPhone::consumer_click(uint16_t usage) {
    if (!connected_ || usage == 0) return;  // 0 is the release report
    std::lock_guard lock(mutex_);
    Phone& p = current();
    auto now = Clock::now();
    if (usage == kConsumerHome) {
        if (now - p.last_home < std::chrono::milliseconds(400)) {
            p.view.event = L"App Switcher";
            log::info("Lab: [{}] App Switcher (double Home)", target_);
            p.last_home = {};
        } else {
            p.view.event = L"Home";
            log::info("Lab: [{}] Home", target_);
            p.last_home = now;
        }
        return;
    }
    p.view.event = std::format(L"Consumer 0x{:04X}", usage);
    log::info("Lab: [{}] consumer 0x{:04X}", target_, usage);
}

LabPhoneView LabPhone::view(const std::string& phone_name) {
    std::lock_guard lock(mutex_);
    Phone& p = phones_[phone_name];
    LabPhoneView v = p.view;
    v.tap_age = p.last_tap == Clock::time_point{} ? 99.0f : std::chrono::duration<float>(Clock::now() - p.last_tap).count();
    v.connected = connected_;
    return v;
}

void LabPhone::expect_pointer(const std::string& phone_name, float x, float y, float tolerance) {
    LabPhoneView v = view(phone_name);
    float err = std::hypot(v.pointer_x - x, v.pointer_y - y);
    if (err <= tolerance) {
        log::info("Lab: PASS [{}] pointer near ({:.2f}, {:.2f}) (error {:.3f})", phone_name, x, y, err);
    } else {
        log::error("Lab: FAIL [{}] pointer at ({:.3f}, {:.3f}), expected ({:.2f}, {:.2f}) +/- {:.2f}", phone_name,
                   v.pointer_x, v.pointer_y, x, y, tolerance);
    }
}

void LabPhone::expect_text(const std::string& phone_name, const std::wstring& text) {
    LabPhoneView v = view(phone_name);
    if (v.text == text) {
        log::info("Lab: PASS [{}] text '{}'", phone_name, log::utf8(text));
    } else {
        log::error("Lab: FAIL [{}] text '{}', expected '{}'", phone_name, log::utf8(v.text), log::utf8(text));
    }
}

}  // namespace pm
