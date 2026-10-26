#pragma once

#include <windows.h>

#include <memory>

#include "input/control_backend.h"

namespace pm {

// The PC's own Bluetooth radio as a BLE HID (HOGP) keyboard + mouse + consumer-control device, via the
// WinRT GATT server. Windows accepts the HID service (0x1812) for third-party GATT servers; only Device
// Information (0x180A) is reserved, which iOS does not require.
class WinBtBackend final : public IControlBackend {
public:
    WinBtBackend();
    ~WinBtBackend() override;
    WinBtBackend(const WinBtBackend&) = delete;
    WinBtBackend& operator=(const WinBtBackend&) = delete;

    void start(HWND notify_window) override;

    bool connected() const override;
    void mouse_move(int dx, int dy) override;
    void mouse_scroll(int wheel, int pan) override;
    void mouse_buttons(uint8_t buttons) override;
    void mouse_steps(int dx, int dy, int step) override;
    void keyboard(const KeyboardState& state) override;
    void consumer_click(uint16_t usage) override;
    void set_target(const std::string& phone_name) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm
