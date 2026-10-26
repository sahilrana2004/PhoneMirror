#include "app/tray.h"

#include <cwchar>

namespace pm {

bool Tray::add(HWND owner, UINT callback_message, HICON icon, const std::wstring& tooltip) {
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = callback_message;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tooltip.c_str(), _TRUNCATE);
    return restore();
}

bool Tray::restore() {
    added_ = Shell_NotifyIconW(NIM_ADD, &data_);
    if (!added_) return false;
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    return true;
}

void Tray::remove() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

}  // namespace pm
