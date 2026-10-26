#include "app/app.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "app/log.h"
#include "app/settings.h"
#include "input/control_backend.h"
#include "lab/lab_phone.h"
#include "lab/lab_runner.h"
#include "mdns/responder.h"
#include "net/firewall.h"
#include "resource.h"
#include "version.h"

namespace pm {
namespace {

constexpr UINT kMsgTray = WM_APP + 2;

enum MenuId : UINT {
    kMenuShowAll = 1,
    kMenuSettings,
    kMenuFirewall,
    kMenuQuit,
    kMenuPhonesBase = 50,   // + count: phones at once
    kMenuWindowBase = 100,  // + slot: toggle that window
};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

int App::run(HINSTANCE instance, const LaunchOptions& options) {
    instance_ = instance;
    log::info("PhoneMirror {} starting", PM_VERSION_STRING);
    Settings& settings = Settings::instance();
    receiver_name_ = widen(settings.get_string("receiver_name", "PhoneMirror"));
    int receivers = std::clamp(static_cast<int>(settings.get_number("receivers", 2)), 1, 4);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = kAppWindowClass;
    RegisterClassExW(&wc);
    // Hidden top-level (not message-only) so it receives the TaskbarCreated broadcast.
    hwnd_ = CreateWindowExW(0, kAppWindowClass, L"PhoneMirror", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        log::error("App window creation failed: {}", GetLastError());
        return 1;
    }
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

    tray_icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!tray_.add(hwnd_, kMsgTray, tray_icon_, L"PhoneMirror — receiver: " + receiver_name_)) {
        log::error("Tray icon could not be added");
    }

    bool lab = !options.lab_scenario.empty();
    bool playing = lab || !options.play_video.empty() || !options.play_audio.empty();
    if (playing) receivers = lab ? LabRunner::phones_in(options.lab_scenario) : 1;
    if (lab) {
        auto phone = std::make_unique<LabPhone>();
        lab_phone_ = phone.get();
        control_ = std::move(phone);
        InputCapture::accept_injected_input(true);
    } else {
        control_ = std::make_unique<WinBtBackend>();
    }
    for (int slot = 0; slot < receivers; ++slot) {
        if (!add_session(slot)) {
            MessageBoxW(nullptr, L"PhoneMirror could not initialize Direct3D 11. See %APPDATA%\\PhoneMirror\\phonemirror.log.",
                        L"PhoneMirror", MB_ICONERROR);
            return 1;
        }
    }
    for (auto& s : sessions_) {
        // Extra receiver slots stay hidden until a phone connects to them (the lab shows every phone).
        if (s->slot() == 0 || lab) s->window().show();
        if (playing) {
            if (!lab) s->start_files(options.play_video, options.play_audio);
        } else {
            s->start_receiver();
        }
    }
    control_->start(hwnd_);
    if (!playing) check_network_setup(hwnd_);
    if (lab) {
        std::vector<Session*> lab_sessions;
        for (auto& s : sessions_) {
            s->window().renderer().set_painter(LabRunner::painter(*lab_phone_, LabRunner::phone_name(s->slot())));
            lab_sessions.push_back(s.get());
        }
        lab_ = std::make_unique<LabRunner>(*lab_phone_, std::move(lab_sessions), hwnd_);
        if (!lab_->load(options.lab_scenario)) return 2;
        lab_->start();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    lab_.reset();
    sessions_.clear();
    MdnsResponder::instance().shutdown();
    log::info("PhoneMirror exiting");
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbar_created_ && taskbar_created_) {
        tray_.restore();
        return 0;
    }
    switch (msg) {
    case kMsgTray:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            show_all();
            return 0;
        case WM_CONTEXTMENU:
            show_tray_menu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            return 0;
        }
        return 0;
    case kMsgShowWindow:
        show_all();
        return 0;
    case kMsgLabQuit:
        quit();
        return 0;
    case kMsgControlState:  // the Bluetooth backend reports here; every window shows the change
        for (auto& s : sessions_) PostMessageW(s->window().hwnd(), kMsgControlState, wp, lp);
        return 0;
    case WM_DESTROY:
        tray_.remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::show_tray_menu(POINT anchor) {
    HMENU menu = CreatePopupMenu();
    std::wstring header = L"Receiver: " + receiver_name_;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (auto& s : sessions_) {
        UINT flags = MF_STRING | (s->window().visible() ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(menu, flags, kMenuWindowBase + s->slot(), s->name().c_str());
    }
    AppendMenuW(menu, MF_STRING, kMenuShowAll, L"Show all windows");
    HMENU phones = CreatePopupMenu();
    for (int n = 1; n <= 4; ++n) {
        UINT checked = static_cast<int>(sessions_.size()) == n ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(phones, MF_STRING | checked, kMenuPhonesBase + n, n == 1 ? L"1 phone" : std::format(L"{} phones", n).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(phones), L"Phones at once");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Open settings file");
    AppendMenuW(menu, MF_STRING, kMenuFirewall, L"Check network && firewall setup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit PhoneMirror");

    SetForegroundWindow(hwnd_);  // required so the menu closes when focus moves away
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, anchor.x, anchor.y, hwnd_,
                                nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kMenuShowAll) show_all();
    if (cmd == kMenuSettings) {
        Settings::instance().save();
        std::filesystem::path path = Settings::data_dir() / L"settings.json";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (cmd == kMenuFirewall) check_network_setup(hwnd_, true);
    if (cmd == kMenuQuit) quit();
    if (cmd > kMenuPhonesBase && cmd <= kMenuPhonesBase + 4) set_phone_count(static_cast<int>(cmd - kMenuPhonesBase));
    if (cmd >= kMenuWindowBase) {
        for (auto& s : sessions_) {
            if (static_cast<int>(cmd - kMenuWindowBase) != s->slot()) continue;
            if (s->window().visible()) {
                s->window().hide();
            } else {
                s->window().show();
            }
        }
    }
}

bool App::add_session(int slot) {
    std::wstring name = slot == 0 ? receiver_name_ : std::format(L"{} ({})", receiver_name_, slot + 1);
    auto session = std::make_unique<Session>(slot, name, control_.get());
    if (!session->create(instance_)) return false;
    session->window().on_activated = [this](int active) { set_audio_focus(active); };
    sessions_.push_back(std::move(session));
    return true;
}

// Tray "Phones at once": adds receivers (each advertises as "PhoneMirror (n)") or removes the last ones live.
void App::set_phone_count(int count) {
    count = std::clamp(count, 1, 4);
    Settings::instance().set_number("receivers", count);
    Settings::instance().save();
    while (static_cast<int>(sessions_.size()) > count) {
        log::info("Receiver \"{}\" removed", log::utf8(sessions_.back()->name()));
        sessions_.pop_back();
    }
    while (static_cast<int>(sessions_.size()) < count) {
        int slot = static_cast<int>(sessions_.size());
        if (!add_session(slot)) break;
        sessions_.back()->start_receiver();
    }
    log::info("Phones at once: {}", sessions_.size());
}

void App::show_all() {
    for (auto& s : sessions_) s->window().show();
}

// With several phones, only the window in front plays sound (setting "audio_follows_focus").
void App::set_audio_focus(int slot) {
    bool follow = sessions_.size() > 1 && Settings::instance().get_bool("audio_follows_focus", true);
    for (auto& s : sessions_) s->audio().set_focus_muted(follow && s->slot() != slot);
}

void App::quit() { DestroyWindow(hwnd_); }

}  // namespace pm
