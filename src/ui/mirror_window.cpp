#include "ui/mirror_window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>

#include "app/log.h"
#include "app/settings.h"
#include "audio/audio_pipeline.h"
#include "record/recorder.h"
#include "record/screenshot.h"
#include "resource.h"
#include "ui/phone_geometry.h"
#include "video/pipeline.h"

namespace pm {
namespace {

constexpr wchar_t kClassName[] = L"PhoneMirror.Mirror";
constexpr int kResizeBorderDip = 8;
constexpr int kTitleZoneDip = 44;
constexpr int kMinHeightDip = 320;
constexpr UINT_PTR kHintTimer = 1;
constexpr UINT_PTR kBarTimer = 2;
constexpr UINT kBarTickMs = 33;
constexpr int kPointerStep = 8;          // counts per report for absolute moves (constant speed)
constexpr int kCalibrationCounts = 400;  // calibration walk from the corner, per axis

// Segoe Fluent Icons / MDL2 Assets code points.
constexpr wchar_t kGlyphHome = 0xE80F;
constexpr wchar_t kGlyphAppSwitcher = 0xE7C4;
constexpr wchar_t kGlyphCamera = 0xE722;
constexpr wchar_t kGlyphRecord = 0xE7C8;
constexpr wchar_t kGlyphStop = 0xE71A;
constexpr wchar_t kGlyphPin = 0xE718;
constexpr wchar_t kGlyphPinned = 0xE840;
constexpr wchar_t kGlyphVolume = 0xE767;
constexpr wchar_t kGlyphMute = 0xE74F;
constexpr wchar_t kGlyphStats = 0xE9D9;
constexpr wchar_t kGlyphSettings = 0xE713;
constexpr wchar_t kGlyphClose = 0xE8BB;

// The window draws its own rounded shape and shadow with per-pixel alpha, so DWM must not add a
// rectangular shadow, border or corner rounding of its own.
void apply_dwm_style(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

bool contains(const geometry::Rect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

MirrorWindow::~MirrorWindow() {
    if (hwnd_) {
        save_placement();
        DestroyWindow(hwnd_);
    }
}

bool MirrorWindow::create(HINSTANCE instance, const std::wstring& receiver_name, int slot) {
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &MirrorWindow::wndproc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }

    slot_ = slot;
    receiver_name_ = receiver_name;
    screen_aspect_ = geometry::kDefaultAspect;
    POINT origin{0, 0};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& work = mi.rcWork;
    int height = static_cast<int>((work.bottom - work.top) * 0.80f);
    int width = height / 2;  // final width once the renderer knows its margin

    // No redirection bitmap: all pixels come from the (composition) swapchain, including transparent ones.
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClassName, receiver_name.c_str(),
                            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX, work.left, work.top, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        log::error("CreateWindowExW failed: {}", GetLastError());
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    apply_dwm_style(hwnd_);

    Settings& s = Settings::instance();
    renderer_.set_tearing_allowed(s.get_bool("tearing", true));
    if (!renderer_.init(hwnd_, dpi_)) return false;
    width = static_cast<int>(std::lround(geometry::client_width_for_height(static_cast<float>(height), screen_aspect_, margin())));
    int x = work.left + ((work.right - work.left) - width) / 2 + slot * 40;
    int y = work.top + ((work.bottom - work.top) - height) / 2 + slot * 40;
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    restore_placement();
    renderer_.set_stats_visible(s.get_bool("stats", false));
    renderer_.set_idle_text(L"Waiting for iPhone…",
                            L"Open Control Center → Screen Mirroring → " + receiver_name_);
    set_topmost(s.get_bool("always_on_top", false));
    SetTimer(hwnd_, kBarTimer, kBarTickMs, nullptr);
    return true;
}

void MirrorWindow::attach(const SessionParts& parts) {
    parts_ = parts;
    capture_.attach(hwnd_, parts.control);
    Settings& s = Settings::instance();
    capture_.configure(s.get_bool("swap_ctrl_cmd", true), s.get_number("pointer_gain", 1.0));
    capture_.on_state = [this](bool on) {
        show_hint(on ? L"Controlling iPhone · Ctrl+Alt to release" : L"", on ? 3000 : 0);
    };
    build_buttons();
}

void MirrorWindow::set_status(std::wstring title, std::wstring detail) {
    renderer_.set_idle_text(std::move(title), std::move(detail));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Follows the stream's orientation: keep the window's long side and centre, swap the short side in.
void MirrorWindow::apply_aspect(float aspect) {
    if (std::fabs(aspect - screen_aspect_) < 0.002f) return;
    screen_aspect_ = aspect;
    renderer_.set_screen_aspect(aspect);
    if (fullscreen_) return;
    capture_.end();
    RECT r;
    GetWindowRect(hwnd_, &r);
    float m = margin();
    float long_side = static_cast<float>(std::max(r.right - r.left, r.bottom - r.top)) - 2 * m;  // screen's long side
    float sw = aspect < 1 ? long_side * aspect : long_side;
    float sh = aspect < 1 ? long_side : long_side / aspect;
    int w = static_cast<int>(std::lround(sw + 2 * m));
    int h = static_cast<int>(std::lround(sh + 2 * m));
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    SetWindowPos(hwnd_, nullptr, cx - w / 2, cy - h / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MirrorWindow::show() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void MirrorWindow::hide() {
    capture_.end();
    save_placement();
    ShowWindow(hwnd_, SW_HIDE);
}

bool MirrorWindow::visible() const { return IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

void MirrorWindow::show_hint(std::wstring text, UINT ms) {
    renderer_.set_overlay(std::move(text));
    if (ms) {
        SetTimer(hwnd_, kHintTimer, ms, nullptr);
    } else {
        KillTimer(hwnd_, kHintTimer);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MirrorWindow::on_click_to_control(POINT pt) {
    if (!parts_.control) return;
    target_this_phone();
    if (capture_.begin()) {
        // Absolute sync (once calibrated): park the pointer in the top-left corner with one huge move, then
        // walk it to the clicked spot in small constant steps so iOS pointer acceleration stays out of it.
        Settings& s = Settings::instance();
        double cal_x = s.get_number("pointer_cal_x", 0);
        double cal_y = s.get_number("pointer_cal_y", 0);
        float nx = 0, ny = 0;
        if (cal_x > 0 && cal_y > 0 && map_to_screen(pt, nx, ny)) {
            parts_.control->mouse_steps(-32767, -32767, 32767);
            parts_.control->mouse_steps(static_cast<int>(nx * cal_x), static_cast<int>(ny * cal_y), kPointerStep);
        }
        return;
    }
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = ARRAYSIZE(pc);
    GetComputerNameW(pc, &len);
    show_hint(std::wstring(L"Keyboard/mouse not connected: on iPhone open Settings › Bluetooth › ") + pc, 5000);
}

// Client pixel -> position on the phone screen as 0..1 fractions; false outside the screen.
bool MirrorWindow::map_to_screen(POINT pt, float& nx, float& ny) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    geometry::Rect s = geometry::screen_rect(static_cast<float>(rc.right), static_cast<float>(rc.bottom),
                                             renderer_.content_aspect(), margin());
    nx = (static_cast<float>(pt.x) - s.x) / s.w;
    ny = (static_cast<float>(pt.y) - s.y) / s.h;
    return nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1;
}

// Ctrl+Shift+P: move the pointer a known number of counts from the corner, user clicks where it landed.
void MirrorWindow::start_calibration() {
    if (!parts_.control || !parts_.control->connected()) {
        on_click_to_control({-1, -1});
        return;
    }
    capture_.end();
    target_this_phone();
    calibrating_ = true;
    parts_.control->mouse_steps(-32767, -32767, 32767);
    parts_.control->mouse_steps(kCalibrationCounts, kCalibrationCounts, kPointerStep);
    show_hint(L"Pointer calibration: when the iPhone pointer stops moving, click exactly on it (Esc cancels)", 20000);
}

void MirrorWindow::finish_calibration(POINT pt) {
    calibrating_ = false;
    float nx = 0, ny = 0;
    if (!map_to_screen(pt, nx, ny) || nx < 0.02f || ny < 0.02f) {
        show_hint(L"Calibration cancelled: click on the pointer inside the phone screen", 3000);
        return;
    }
    // Counts needed to cross the whole screen at the calibration step size.
    double cal_x = kCalibrationCounts / nx;
    double cal_y = kCalibrationCounts / ny;
    Settings& s = Settings::instance();
    s.set_number("pointer_cal_x", cal_x);
    s.set_number("pointer_cal_y", cal_y);
    s.save();
    log::info("Pointer calibrated: {:.0f} x {:.0f} counts per screen", cal_x, cal_y);
    show_hint(L"Pointer calibrated · clicking the screen now puts the pointer where you click", 3000);
}

void MirrorWindow::phone_action(uint16_t usage, int presses) {
    if (!parts_.control || !parts_.control->connected()) {
        on_click_to_control({-1, -1});  // not connected: shows how to connect
        return;
    }
    target_this_phone();
    for (int i = 0; i < presses; ++i) parts_.control->consumer_click(usage);
}

// ---- hover bar -------------------------------------------------------------------------------------------

void MirrorWindow::build_buttons() {
    bool recording = parts_.recorder && parts_.recorder->active();
    bool muted = parts_.audio && parts_.audio->muted();
    bar_.buttons = {
        {BarAction::Home, kGlyphHome},
        {BarAction::AppSwitcher, kGlyphAppSwitcher},
        {BarAction::Screenshot, kGlyphCamera},
        {BarAction::Record, recording ? kGlyphStop : kGlyphRecord, recording},
        {BarAction::Pin, topmost_ ? kGlyphPinned : kGlyphPin, topmost_},
        {BarAction::Mute, muted ? kGlyphMute : kGlyphVolume, muted},
        {BarAction::Stats, kGlyphStats, renderer_.stats_visible()},
        {BarAction::Settings, kGlyphSettings},
        {BarAction::Close, kGlyphClose},
    };
}

BarLayout MirrorWindow::layout() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float scale = 96.0f / static_cast<float>(dpi_);
    return hover_bar_layout(rc.right * scale, rc.bottom * scale, const_cast<Renderer&>(renderer_).content_aspect(),
                            renderer_.margin_px() * scale, bar_.buttons.size());
}

int MirrorWindow::button_at(POINT pt) const {
    if (bar_.opacity < 0.5f) return -1;
    float scale = 96.0f / static_cast<float>(dpi_);
    BarLayout l = layout();
    for (size_t i = 0; i < l.buttons.size(); ++i) {
        if (contains(l.buttons[i], pt.x * scale, pt.y * scale)) return static_cast<int>(i);
    }
    return -1;
}

// Polled at ~30 Hz: reveal the bar while the cursor is near the top of the window, fade otherwise.
void MirrorWindow::update_bar(bool animate) {
    POINT cursor;
    GetCursorPos(&cursor);
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    bool inside = PtInRect(&wr, cursor) && visible() && !capture_.active();
    POINT client = cursor;
    ScreenToClient(hwnd_, &client);
    bool near_top = inside && client.y < static_cast<int>(margin()) + dip(kTitleZoneDip) * 2;
    bar_target_ = near_top ? 1.0f : 0.0f;

    float before = bar_.opacity;
    int hovered_before = bar_.hovered;
    if (animate) {
        float step = kBarTickMs / 150.0f;  // 150 ms fade
        bar_.opacity = bar_target_ > bar_.opacity ? std::min(1.0f, bar_.opacity + step) : std::max(0.0f, bar_.opacity - step);
    }
    bar_.hovered = near_top ? button_at(client) : -1;

    std::wstring time;
    if (parts_.recorder && parts_.recorder->active()) {
        int secs = static_cast<int>(parts_.recorder->elapsed_seconds());
        time = std::format(L"{:02}:{:02}", secs / 60, secs % 60);
    }
    bool recording = !time.empty();
    std::wstring title = parts_.video && !parts_.video->client_name().empty() ? widen(parts_.video->client_name())
                                                                               : receiver_name_;
    bool changed = before != bar_.opacity || hovered_before != bar_.hovered || bar_.record_time != time ||
                   bar_.recording != recording || bar_.title != title;
    if (!changed) return;
    bar_.record_time = std::move(time);
    bar_.recording = recording;
    bar_.title = std::move(title);
    renderer_.set_hover_bar(bar_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MirrorWindow::run_action(BarAction action) {
    switch (action) {
    case BarAction::Home: phone_action(kConsumerHome, 1); break;
    case BarAction::AppSwitcher: phone_action(kConsumerHome, 2); break;
    case BarAction::Screenshot: take_screenshot(); break;
    case BarAction::Record: toggle_recording(); break;
    case BarAction::Pin:
        set_topmost(!topmost_);
        Settings::instance().set_bool("always_on_top", topmost_);
        Settings::instance().save();
        break;
    case BarAction::Mute:
        if (parts_.audio) {
            parts_.audio->set_muted(!parts_.audio->muted());
            show_hint(parts_.audio->muted() ? L"Muted" : L"Sound on", 1200);
        }
        break;
    case BarAction::Stats:
        renderer_.set_stats_visible(!renderer_.stats_visible());
        Settings::instance().set_bool("stats", renderer_.stats_visible());
        Settings::instance().save();
        break;
    case BarAction::Settings: {
        Settings::instance().save();  // make sure the file exists with current values
        std::filesystem::path path = Settings::data_dir() / L"settings.json";
        ShellExecuteW(hwnd_, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
        show_hint(L"Settings apply on next start", 3000);
        break;
    }
    case BarAction::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    }
    static constexpr const char* kNames[] = {"home", "app switcher", "screenshot", "record", "pin", "mute", "stats", "settings", "close"};
    log::info("UI: {} (pin {}, mute {}, stats {})", kNames[static_cast<int>(action)], topmost_ ? "on" : "off",
              parts_.audio && parts_.audio->muted() ? "on" : "off", renderer_.stats_visible() ? "on" : "off");
    build_buttons();
    renderer_.set_hover_bar(bar_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- actions ----------------------------------------------------------------------------------------------

void MirrorWindow::take_screenshot() {
    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!renderer_.capture_frame(pixels, w, h)) {
        show_hint(L"Nothing to capture yet", 1500);
        return;
    }
    std::filesystem::path path = media_folder(FOLDERID_Pictures) / (timestamped_stem(L"PhoneMirror") + L".png");
    bool saved = save_png(path, pixels, w, h);
    if (Settings::instance().get_bool("screenshot_to_clipboard", true)) copy_image_to_clipboard(hwnd_, pixels, w, h);
    log::info("Screenshot {}x{} -> {} ({})", w, h, log::utf8(path.wstring()), saved ? "ok" : "failed");
    show_hint(saved ? L"Screenshot saved to Pictures\\PhoneMirror" : L"Screenshot failed", 2000);
}

void MirrorWindow::toggle_recording() {
    if (!parts_.recorder) return;
    if (parts_.recorder->active()) {
        parts_.recorder->stop();
        show_hint(L"Recording saved to Videos\\PhoneMirror", 2500);
    } else {
        parts_.recorder->start(media_folder(FOLDERID_Videos), timestamped_stem(L"PhoneMirror"));
        show_hint(L"Recording…", 1500);
    }
    build_buttons();
    renderer_.set_hover_bar(bar_);
}

void MirrorWindow::set_topmost(bool topmost) {
    topmost_ = topmost;
    SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// F11: cover the monitor (the flip-model swapchain then gets independent flip, the lowest-latency path).
void MirrorWindow::toggle_fullscreen() {
    capture_.end();
    if (!fullscreen_) {
        GetWindowRect(hwnd_, &windowed_rect_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        fullscreen_ = true;
        renderer_.set_fullscreen(true);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        fullscreen_ = false;
        renderer_.set_fullscreen(false);
        const RECT& r = windowed_rect_;
        SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED);
    }
    log::info("UI: fullscreen {}", fullscreen_ ? "on" : "off");
}

bool MirrorWindow::on_shortcut(UINT vk, bool ctrl, bool shift) {
    if (vk == VK_F3) {
        run_action(BarAction::Stats);
    } else if (vk == VK_F11) {
        toggle_fullscreen();
    } else if (ctrl && shift && vk == 'S') {
        take_screenshot();
    } else if (ctrl && shift && vk == 'R') {
        toggle_recording();
    } else if (ctrl && shift && vk == 'P') {
        start_calibration();
    } else if (vk == VK_ESCAPE && calibrating_) {
        calibrating_ = false;
        show_hint(L"", 0);
    } else if (ctrl && !shift && vk == '1') {
        phone_action(kConsumerHome, 1);
    } else if (ctrl && !shift && vk == '2') {
        phone_action(kConsumerHome, 2);
    } else if (ctrl && !shift && vk == '3') {
        if (parts_.control && parts_.control->connected()) {
            target_this_phone();
            parts_.control->keyboard({0x08, {0x2C}});  // Command+Space
            parts_.control->keyboard({});
        } else {
            on_click_to_control({-1, -1});
        }
    } else if (vk == VK_ESCAPE && fullscreen_) {
        toggle_fullscreen();
    } else {
        return false;
    }
    return true;
}

void MirrorWindow::save_placement() const {
    if (!hwnd_ || fullscreen_ || IsIconic(hwnd_)) return;
    RECT r;
    GetWindowRect(hwnd_, &r);
    Settings& s = Settings::instance();
    s.set_string(std::format("window.{}", slot_), std::format("{},{},{}", r.left, r.top, r.bottom - r.top));
    s.save();
}

// Height and position are remembered per slot; width follows the content aspect.
void MirrorWindow::restore_placement() {
    std::string saved = Settings::instance().get_string(std::format("window.{}", slot_), "");
    int x = 0, y = 0, h = 0;
    if (saved.empty() || sscanf_s(saved.c_str(), "%d,%d,%d", &x, &y, &h) != 3 || h < 200) return;
    int w = static_cast<int>(std::lround(geometry::client_width_for_height(static_cast<float>(h), screen_aspect_, margin())));
    RECT r{x, y, x + w, y + h};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    RECT visible_part;
    if (!IntersectRect(&visible_part, &r, &mi.rcWork)) return;  // monitor gone: keep the centred default
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- window procedure ------------------------------------------------------------------------------------

LRESULT CALLBACK MirrorWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<MirrorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MirrorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MirrorWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (LRESULT captured = 0; capture_.handle(msg, wp, lp, captured)) return captured;
    switch (msg) {
    case WM_NCCALCSIZE:
        return 0;  // client area covers the whole window, including the creation-time call with wp == FALSE
    case WM_NCHITTEST:
        return hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) return 0;  // no maximize on title double-click
        break;
    case WM_SIZING:
        if (!fullscreen_) on_sizing(wp, reinterpret_cast<RECT*>(lp));
        return TRUE;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        int min_h = dip(kMinHeightDip);
        mmi->ptMinTrackSize = {static_cast<LONG>(std::lround(geometry::client_width_for_height(static_cast<float>(min_h), screen_aspect_, margin()))), min_h};
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            renderer_.resize();
            renderer_.render();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        save_placement();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        renderer_.set_dpi(dpi_);
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && on_activated) on_activated(slot_);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        renderer_.render();
        ValidateRect(hwnd_, nullptr);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (calibrating_) {
            finish_calibration(pt);
            return 0;
        }
        pressed_ = button_at(pt);
        if (pressed_ < 0) on_click_to_control(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = button_at(pt);
        if (idx >= 0 && idx == pressed_ && idx < static_cast<int>(bar_.buttons.size())) run_action(bar_.buttons[idx].action);
        pressed_ = -1;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        int idx = button_at(pt);
        if (idx >= 0 && bar_.buttons[idx].action == BarAction::Mute && parts_.audio) {
            float v = parts_.audio->local_volume() + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.05f : -0.05f);
            v = std::clamp(v, 0.0f, 1.0f);
            parts_.audio->set_local_volume(v);
            show_hint(std::format(L"Volume {}%", static_cast<int>(std::lround(v * 100))), 1000);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (on_shortcut(static_cast<UINT>(wp), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0)) return 0;
        break;
    case kMsgShortcut:
        on_shortcut(static_cast<UINT>(wp), true, true);  // only reachable for F3/F11/Ctrl+Shift+S/R
        return 0;
    case WM_TIMER:
        if (wp == kHintTimer) {
            show_hint(L"", 0);
        } else if (wp == kBarTimer) {
            update_bar(true);
        }
        return 0;
    case kMsgControlState:
        if (wp) {
            show_hint(L"iPhone keyboard & mouse connected · click to control", 3000);
        } else {
            capture_.end();
            show_hint(L"iPhone keyboard & mouse disconnected", 3000);
        }
        return 0;
    case WM_CLOSE:
        if (fullscreen_) toggle_fullscreen();
        hide();  // the receiver keeps running in the tray
        return 0;
    case kMsgMirrorStarted:
        show();
        return 0;
    case kMsgMirrorStopped:
        renderer_.clear_frame();
        if (parts_.recorder && parts_.recorder->active()) toggle_recording();
        set_status(L"iPhone disconnected",
                   L"Reconnect from Control Center → Screen Mirroring → " + receiver_name_);
        return 0;
    case kMsgVideoSize:
        if (wp && lp) apply_aspect(static_cast<float>(wp) / static_cast<float>(lp));
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT MirrorWindow::hit_test(POINT pt) const {
    if (fullscreen_) return HTCLIENT;
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int b = static_cast<int>(margin()) + dip(kResizeBorderDip);  // the transparent shadow margin plus a few px of the screen
    bool left = pt.x < rc.left + b;
    bool right = pt.x >= rc.right - b;
    bool top = pt.y < rc.top + b;
    bool bottom = pt.y >= rc.bottom - b;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    POINT client = pt;
    ScreenToClient(hwnd_, &client);
    if (button_at(client) >= 0) return HTCLIENT;
    if (pt.y < rc.top + static_cast<int>(margin()) + dip(kTitleZoneDip)) return HTCAPTION;
    return HTCLIENT;
}

void MirrorWindow::on_sizing(WPARAM edge, RECT* r) const {
    float a = screen_aspect_;
    float m = margin();
    auto height_for = [&](int w) { return static_cast<int>(std::lround(geometry::client_height_for_width(static_cast<float>(w), a, m))); };
    auto width_for = [&](int h) { return static_cast<int>(std::lround(geometry::client_width_for_height(static_cast<float>(h), a, m))); };
    int w = r->right - r->left;
    int h = r->bottom - r->top;
    switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
        r->bottom = r->top + height_for(w);
        break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
        r->right = r->left + width_for(h);
        break;
    case WMSZ_TOPLEFT:
    case WMSZ_TOPRIGHT:
        r->top = r->bottom - height_for(w);
        break;
    default:
        r->bottom = r->top + height_for(w);
        break;
    }
}

}  // namespace pm

namespace pm {

// With several phones paired to the PC's Bluetooth keyboard/mouse, input follows the window being used.
void MirrorWindow::target_this_phone() {
    if (parts_.control) parts_.control->set_target(parts_.video ? parts_.video->client_name() : std::string());
}

}  // namespace pm
