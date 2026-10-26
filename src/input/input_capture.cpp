#include "input/input_capture.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "app/log.h"
#include "input/keymap.h"

namespace pm {
namespace {

InputCapture* g_capture = nullptr;  // the hook is process-wide; only one window captures at a time

constexpr uint8_t kCtrlBits = 0x11;
constexpr uint8_t kAltBits = 0x44;
constexpr uint8_t kGuiBits = 0x88;

// Ctrl <-> GUI swap per side: LCtrl(0x01)<->LGUI(0x08), RCtrl(0x10)<->RGUI(0x80).
uint8_t swap_ctrl_gui(uint8_t m) {
    uint8_t out = m & ~(kCtrlBits | kGuiBits);
    if (m & 0x01) out |= 0x08;
    if (m & 0x08) out |= 0x01;
    if (m & 0x10) out |= 0x80;
    if (m & 0x80) out |= 0x10;
    return out;
}

}  // namespace

bool InputCapture::begin() {
    if (active_ || !backend_ || !backend_->connected()) return false;
    RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd_};  // generic desktop / mouse, foreground only
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        log::error("Raw input registration failed: {}", GetLastError());
        return false;
    }
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputCapture::keyboard_hook, GetModuleHandleW(nullptr), 0);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    ClipCursor(&rc);
    g_capture = this;
    active_ = true;
    keyboard_ = {};
    physical_mods_ = 0;
    buttons_ = 0;
    carry_x_ = carry_y_ = 0;
    last_abs_x_ = last_abs_y_ = -1;
    SetCursor(nullptr);
    log::info("Control: capture on");
    if (on_state) on_state(true);
    return true;
}

void InputCapture::end() {
    if (!active_) return;
    release_everything();
    if (hook_) UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ClipCursor(nullptr);
    if (g_capture == this) g_capture = nullptr;
    active_ = false;
    log::info("Control: capture off");
    if (on_state) on_state(false);
}

void InputCapture::release_everything() {
    if (!backend_) return;
    if (keyboard_.modifiers || keyboard_.keys[0]) {
        keyboard_ = {};
        backend_->keyboard(keyboard_);
    }
    if (buttons_) {
        buttons_ = 0;
        backend_->mouse_buttons(0);
    }
}

bool InputCapture::handle(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!active_) return false;
    switch (msg) {
    case WM_INPUT:
        on_raw_input(reinterpret_cast<HRAWINPUT>(lp));
        result = DefWindowProcW(hwnd_, msg, wp, lp);
        return true;
    case WM_SETCURSOR:
        SetCursor(nullptr);
        result = TRUE;
        return true;
    case WM_NCHITTEST:  // no dragging/resizing while the mouse belongs to the phone
        result = HTCLIENT;
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_MOUSEMOVE:
        result = 0;  // raw input carries these to the phone
        return true;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) end();
        return false;
    case WM_CANCELMODE:
    case kMsgReleaseCapture:
        end();
        result = 0;
        return msg == kMsgReleaseCapture;
    }
    return false;
}

void InputCapture::on_raw_input(HRAWINPUT input) {
    RAWINPUT raw;
    UINT size = sizeof(raw);
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    if (raw.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = raw.data.mouse;

    LONG dx = m.lLastX;
    LONG dy = m.lLastY;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {  // remote desktop / tablets report positions, not deltas
        if (last_abs_x_ < 0) {
            dx = dy = 0;
        } else {
            int w = GetSystemMetrics((m.usFlags & MOUSE_VIRTUAL_DESKTOP) ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            int h = GetSystemMetrics((m.usFlags & MOUSE_VIRTUAL_DESKTOP) ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            dx = MulDiv(m.lLastX - last_abs_x_, w, 65535);
            dy = MulDiv(m.lLastY - last_abs_y_, h, 65535);
        }
        last_abs_x_ = m.lLastX;
        last_abs_y_ = m.lLastY;
    }
    if (dx || dy) {
        carry_x_ += dx * pointer_gain_;
        carry_y_ += dy * pointer_gain_;
        auto ix = static_cast<int>(std::trunc(carry_x_));
        auto iy = static_cast<int>(std::trunc(carry_y_));
        carry_x_ -= ix;
        carry_y_ -= iy;
        if (ix || iy) backend_->mouse_move(ix, iy);
    }

    USHORT f = m.usButtonFlags;
    uint8_t buttons = buttons_;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) buttons |= 0x01;
    if (f & RI_MOUSE_LEFT_BUTTON_UP) buttons &= ~0x01;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) buttons |= 0x02;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP) buttons &= ~0x02;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) buttons |= 0x04;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP) buttons &= ~0x04;
    if (buttons != buttons_) {
        buttons_ = buttons;
        backend_->mouse_buttons(buttons);
    }

    // One wheel detent (120) = one HID wheel step; high-resolution wheels accumulate.
    if (f & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
        int& carry = (f & RI_MOUSE_WHEEL) ? wheel_carry_ : pan_carry_;
        carry += static_cast<SHORT>(m.usButtonData);
        int steps = carry / WHEEL_DELTA;
        carry -= steps * WHEEL_DELTA;
        if (steps) {
            if (f & RI_MOUSE_WHEEL) {
                backend_->mouse_scroll(steps, 0);
            } else {
                backend_->mouse_scroll(0, steps);
            }
        }
    }
}

LRESULT CALLBACK InputCapture::keyboard_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capture && g_capture->active_) {
        const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (!(k.flags & LLKHF_INJECTED) && g_capture->on_key(k, down)) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Returns true when the key is swallowed (sent to the phone or used as a local shortcut).
bool InputCapture::on_key(const KBDLLHOOKSTRUCT& k, bool down) {
    if (uint16_t usage = consumer_usage_for_vk(k.vkCode)) {
        if (down) backend_->consumer_click(usage);
        return true;
    }
    uint8_t mod = 0;
    uint8_t usage = hid_usage_for_scan(k.scanCode, (k.flags & LLKHF_EXTENDED) != 0, &mod);

    if (mod) {
        physical_mods_ = down ? (physical_mods_ | mod) : (physical_mods_ & ~mod);
        // Escape hatch: Ctrl+Alt hands mouse and keyboard back to Windows.
        if ((physical_mods_ & kCtrlBits) && (physical_mods_ & kAltBits)) {
            PostMessageW(hwnd_, kMsgReleaseCapture, 0, 0);
            return true;
        }
        uint8_t mapped = swap_ctrl_cmd_ ? swap_ctrl_gui(mod) : mod;
        keyboard_.modifiers = down ? (keyboard_.modifiers | mapped) : (keyboard_.modifiers & ~mapped);
        send_keyboard();
        return true;
    }
    if (!usage) return true;  // unmapped keys are not passed to Windows either while captured

    // Ctrl+1 Home, Ctrl+2 App Switcher, Ctrl+3 Spotlight (macOS iPhone Mirroring uses Cmd+1/2/3).
    if ((physical_mods_ & kCtrlBits) && !(physical_mods_ & ~kCtrlBits) && usage >= 0x1E && usage <= 0x20) {
        if (!down) return true;
        uint8_t held = keyboard_.modifiers;
        if (usage == 0x1E) {
            backend_->consumer_click(kConsumerHome);
        } else if (usage == 0x1F) {
            backend_->consumer_click(kConsumerHome);  // double Home press opens the App Switcher
            backend_->consumer_click(kConsumerHome);
        } else {
            KeyboardState spotlight{0x08, {0x2C}};  // Command+Space
            backend_->keyboard({});
            backend_->keyboard(spotlight);
            backend_->keyboard({});
            backend_->keyboard({held, {}});
        }
        return true;
    }

    uint8_t* end = std::end(keyboard_.keys);
    uint8_t* found = std::find(std::begin(keyboard_.keys), end, usage);
    if (down && found == end) {
        uint8_t* slot = std::find(std::begin(keyboard_.keys), end, static_cast<uint8_t>(0));
        if (slot == end) return true;  // 6-key rollover exhausted
        *slot = usage;
        send_keyboard();
    } else if (!down && found != end) {
        std::copy(found + 1, end, found);
        keyboard_.keys[5] = 0;
        send_keyboard();
    }
    return true;
}

void InputCapture::send_keyboard() { backend_->keyboard(keyboard_); }

}  // namespace pm
