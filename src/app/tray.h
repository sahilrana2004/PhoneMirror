#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace pm {

class Tray {
public:
    Tray() = default;
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    ~Tray() { remove(); }

    bool add(HWND owner, UINT callback_message, HICON icon, const std::wstring& tooltip);
    bool restore();  // re-add after Explorer restarts
    void remove();

private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};

}  // namespace pm
