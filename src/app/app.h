#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/session.h"
#include "app/tray.h"
#include "input/winbt_backend.h"
#include "lab/lab_phone.h"
#include "lab/lab_runner.h"

namespace pm {

inline constexpr wchar_t kAppWindowClass[] = L"PhoneMirror.App";
inline constexpr UINT kMsgShowWindow = WM_APP + 1;  // sent by a second instance

struct LaunchOptions {
    std::filesystem::path play_video;
    std::filesystem::path play_audio;
    std::filesystem::path lab_scenario;  // --lab: virtual iPhone + scripted scenario, no network
};

class App {
public:
    int run(HINSTANCE instance, const LaunchOptions& options);

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void show_tray_menu(POINT anchor);
    void show_all();
    bool add_session(int slot);
    void set_phone_count(int count);
    void set_audio_focus(int slot);
    void quit();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    HICON tray_icon_ = nullptr;
    std::wstring receiver_name_;
    Tray tray_;
    // One keyboard/mouse link (the PC's Bluetooth, or the lab's virtual iPhone), used by the focused window.
    std::unique_ptr<IControlBackend> control_;
    LabPhone* lab_phone_ = nullptr;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::unique_ptr<LabRunner> lab_;
};

}  // namespace pm
