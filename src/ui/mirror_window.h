#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "input/input_capture.h"
#include "ui/hover_bar.h"
#include "video/renderer.h"

namespace pm {

class AudioPipeline;
class Recorder;
class VideoPipeline;

// The parts of a session the window's controls act on.
struct SessionParts {
    int slot = 0;
    VideoPipeline* video = nullptr;
    AudioPipeline* audio = nullptr;
    Recorder* recorder = nullptr;
    IControlBackend* control = nullptr;
};

// Borderless phone-shaped window in the style of macOS iPhone Mirroring: aspect-locked, follows rotation,
// hover-revealed title bar (Home, App Switcher, screenshot, record, pin, mute, stats, settings, close).
class MirrorWindow {
public:
    MirrorWindow() = default;
    MirrorWindow(const MirrorWindow&) = delete;
    MirrorWindow& operator=(const MirrorWindow&) = delete;
    ~MirrorWindow();

    bool create(HINSTANCE instance, const std::wstring& receiver_name, int slot);
    void attach(const SessionParts& parts);
    void show();
    void hide();
    bool visible() const;
    HWND hwnd() const { return hwnd_; }
    Renderer& renderer() { return renderer_; }
    void set_status(std::wstring title, std::wstring detail);

    std::function<void(int slot)> on_activated;  // audio focus follows the active window

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT hit_test(POINT screen_pt) const;
    void on_sizing(WPARAM edge, RECT* rect) const;
    void apply_aspect(float aspect);
    int dip(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    void show_hint(std::wstring text, UINT ms);
    void on_click_to_control();
    bool on_shortcut(UINT vk, bool ctrl, bool shift);

    // Hover bar.
    void build_buttons();
    BarLayout layout() const;
    int button_at(POINT client_pt) const;
    void update_bar(bool animate);
    void run_action(BarAction action);

    void take_screenshot();
    void toggle_recording();
    void toggle_fullscreen();
    void set_topmost(bool topmost);
    void phone_action(uint16_t consumer_usage, int presses);
    void save_placement() const;
    void restore_placement();

    HWND hwnd_ = nullptr;
    std::wstring receiver_name_;
    int slot_ = 0;
    UINT dpi_ = 96;
    float screen_aspect_ = 0;
    Renderer renderer_;
    InputCapture capture_;
    SessionParts parts_;

    HoverBarState bar_;
    float bar_target_ = 0;
    int pressed_ = -1;
    bool topmost_ = false;
    bool fullscreen_ = false;
    RECT windowed_rect_{};
};

}  // namespace pm
