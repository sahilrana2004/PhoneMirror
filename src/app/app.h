#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/session.h"
#include "app/tray.h"
#include "input/winbt_backend.h"

namespace pm {

inline constexpr wchar_t kAppWindowClass[] = L"PhoneMirror.App";
inline constexpr UINT kMsgShowWindow = WM_APP + 1;  // sent by a second instance

struct LaunchOptions {
    std::filesystem::path play_video;
    std::filesystem::path play_audio;
};

class App {
public:
    int run(HINSTANCE instance, const LaunchOptions& options);

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void show_tray_menu(POINT anchor);
    void show_all();
    void set_audio_focus(int slot);
    void quit();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    HICON tray_icon_ = nullptr;
    std::wstring receiver_name_;
    Tray tray_;
    WinBtBackend control_;  // one Bluetooth keyboard/mouse, used by whichever window has focus
    std::vector<std::unique_ptr<Session>> sessions_;
};

}  // namespace pm
