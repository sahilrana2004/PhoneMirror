#pragma once

#include <windows.h>

#include <string>

#include "video/renderer.h"

namespace pm {

class MirrorWindow {
public:
    MirrorWindow() = default;
    MirrorWindow(const MirrorWindow&) = delete;
    MirrorWindow& operator=(const MirrorWindow&) = delete;
    ~MirrorWindow();

    bool create(HINSTANCE instance, const std::wstring& receiver_name);
    void show();
    void hide();
    bool visible() const;
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT hit_test(POINT screen_pt) const;
    void on_sizing(WPARAM edge, RECT* rect) const;
    int dip(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float screen_aspect_ = 0;
    Renderer renderer_;
};

}  // namespace pm
