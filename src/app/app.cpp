#include "app/app.h"

#include <windowsx.h>

#include "app/log.h"
#include "mdns/responder.h"
#include "resource.h"
#include "version.h"

namespace pm {
namespace {

constexpr UINT kMsgTray = WM_APP + 2;

enum MenuId : UINT {
    kMenuToggle = 1,
    kMenuQuit,
};

}  // namespace

int App::run(HINSTANCE instance) {
    instance_ = instance;
    log::info("PhoneMirror {} starting", PM_VERSION_STRING);

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

    if (!mirror_.create(instance, receiver_name_)) {
        MessageBoxW(nullptr, L"PhoneMirror could not initialize Direct3D 11. See %APPDATA%\\PhoneMirror\\phonemirror.log.",
                    L"PhoneMirror", MB_ICONERROR);
        return 1;
    }
    mirror_.show();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring play;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--play") play = argv[i + 1];
    }
    LocalFree(argv);
    if (play.empty()) {
        start_receiver();
    } else {
        pipeline_ = std::make_unique<VideoPipeline>(mirror_.renderer(), mirror_.hwnd());
        file_source_.start(play, *pipeline_);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    file_source_.stop();
    receiver_.reset();
    pipeline_.reset();
    MdnsResponder::instance().shutdown();
    log::info("PhoneMirror exiting");
    return static_cast<int>(msg.wParam);
}

void App::start_receiver() {
    pipeline_ = std::make_unique<VideoPipeline>(mirror_.renderer(), mirror_.hwnd());
    receiver_ = std::make_unique<Receiver>(*pipeline_);
    ReceiverConfig config;
    config.name = log::utf8(receiver_name_);
    // iOS sizes the stream from the requested height and only exceeds 1080 lines over H.265.
    config.h265 = mirror_.renderer().supports_hevc();
    config.width = config.h265 ? 3840 : 1920;
    config.height = config.h265 ? 2160 : 1080;
    if (!receiver_->start(config)) {
        receiver_.reset();
        mirror_.set_status(L"AirPlay receiver failed to start",
                           L"Another AirPlay receiver may be using TCP port 7000. Details in %APPDATA%\\PhoneMirror\\phonemirror.log");
    }
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
            toggle_mirror();
            return 0;
        case WM_CONTEXTMENU:
            show_tray_menu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            return 0;
        }
        return 0;
    case kMsgShowWindow:
        mirror_.show();
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
    AppendMenuW(menu, MF_STRING, kMenuToggle, mirror_.visible() ? L"Hide window" : L"Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit PhoneMirror");

    SetForegroundWindow(hwnd_);  // required so the menu closes when focus moves away
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, anchor.x, anchor.y, hwnd_,
                                nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kMenuToggle) toggle_mirror();
    if (cmd == kMenuQuit) quit();
}

void App::toggle_mirror() {
    if (mirror_.visible()) {
        mirror_.hide();
    } else {
        mirror_.show();
    }
}

void App::quit() { DestroyWindow(hwnd_); }

}  // namespace pm
