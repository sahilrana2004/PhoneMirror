#include "ui/mirror_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <cmath>

#include "app/log.h"
#include "resource.h"
#include "ui/phone_geometry.h"

namespace pm {
namespace {

constexpr wchar_t kClassName[] = L"PhoneMirror.Mirror";
constexpr int kResizeBorderDip = 8;
constexpr int kTitleZoneDip = 44;
constexpr int kMinHeightDip = 320;

void apply_dwm_style(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    // A non-zero frame margin keeps the DWM drop shadow on a window without a non-client area.
    MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}

}  // namespace

MirrorWindow::~MirrorWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool MirrorWindow::create(HINSTANCE instance, const std::wstring& receiver_name) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MirrorWindow::wndproc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    screen_aspect_ = geometry::kDefaultAspect;
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const RECT& work = mi.rcWork;
    int height = static_cast<int>((work.bottom - work.top) * 0.78f);
    int width = static_cast<int>(std::lround(height * geometry::client_aspect(screen_aspect_)));
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + ((work.bottom - work.top) - height) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"PhoneMirror",
                            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX, x, y, width, height, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) {
        log::error("CreateWindowExW failed: {}", GetLastError());
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    apply_dwm_style(hwnd_);

    if (!renderer_.init(hwnd_, dpi_)) return false;
    renderer_.set_idle_text(L"Waiting for iPhone…",
                            L"Open Control Center → Screen Mirroring → " + receiver_name);
    return true;
}

void MirrorWindow::show() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void MirrorWindow::hide() { ShowWindow(hwnd_, SW_HIDE); }

bool MirrorWindow::visible() const { return IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

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
    switch (msg) {
    case WM_NCCALCSIZE:
        return 0;  // client area covers the whole window, including the creation-time call with wp == FALSE
    case WM_NCHITTEST:
        return hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) return 0;  // no maximize on title double-click
        break;
    case WM_SIZING:
        on_sizing(wp, reinterpret_cast<RECT*>(lp));
        return TRUE;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        int min_h = dip(kMinHeightDip);
        mmi->ptMinTrackSize = {static_cast<LONG>(std::lround(min_h * geometry::client_aspect(screen_aspect_))), min_h};
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            renderer_.resize();
            renderer_.render();
        }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        renderer_.set_dpi(dpi_);
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        renderer_.render();
        ValidateRect(hwnd_, nullptr);
        return 0;
    case WM_CLOSE:
        hide();  // the receiver keeps running in the tray
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT MirrorWindow::hit_test(POINT pt) const {
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int b = dip(kResizeBorderDip);
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
    if (pt.y < rc.top + dip(kTitleZoneDip)) return HTCAPTION;
    return HTCLIENT;
}

void MirrorWindow::on_sizing(WPARAM edge, RECT* r) const {
    float aspect = geometry::client_aspect(screen_aspect_);
    int w = r->right - r->left;
    int h = r->bottom - r->top;
    switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
        r->bottom = r->top + static_cast<int>(std::lround(w / aspect));
        break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
        r->right = r->left + static_cast<int>(std::lround(h * aspect));
        break;
    case WMSZ_TOPLEFT:
    case WMSZ_TOPRIGHT:
        r->top = r->bottom - static_cast<int>(std::lround(w / aspect));
        break;
    default:
        r->bottom = r->top + static_cast<int>(std::lround(w / aspect));
        break;
    }
}

}  // namespace pm
