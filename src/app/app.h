#pragma once

#include <windows.h>

#include <string>

#include "app/tray.h"
#include "ui/mirror_window.h"

namespace pm {

inline constexpr wchar_t kAppWindowClass[] = L"PhoneMirror.App";
inline constexpr UINT kMsgShowWindow = WM_APP + 1;  // sent by a second instance

class App {
public:
    int run(HINSTANCE instance);

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void show_tray_menu(POINT anchor);
    void toggle_mirror();
    void quit();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    HICON tray_icon_ = nullptr;
    std::wstring receiver_name_ = L"PhoneMirror";
    Tray tray_;
    MirrorWindow mirror_;
};

}  // namespace pm
