#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "ui/phone_geometry.h"

namespace pm {

enum class BarAction { Home, AppSwitcher, Screenshot, Record, Pin, Mute, Stats, Settings, Close };

struct BarButton {
    BarAction action;
    wchar_t glyph;  // Segoe Fluent Icons / Segoe MDL2 Assets code point
    bool active = false;
};

// Everything the renderer needs to draw the hover title bar; built by the window on the UI thread.
struct HoverBarState {
    float opacity = 0;  // 0 hidden .. 1 fully shown
    std::wstring title;
    std::vector<BarButton> buttons;
    int hovered = -1;
    bool recording = false;
    std::wstring record_time;  // "01:23" while recording
};

struct BarLayout {
    geometry::Rect bar;
    geometry::Rect title;
    std::vector<geometry::Rect> buttons;
};

inline constexpr float kBarHeightDip = 40.0f;

// Layout in DIPs for a client of the given DIP size showing content of the given aspect. The bar spans the
// top of the phone screen, inset from the rounded corners; buttons shrink before they would overflow.
inline BarLayout hover_bar_layout(float client_w, float client_h, float aspect, float margin, size_t button_count) {
    geometry::Rect s = geometry::screen_rect(client_w, client_h, aspect, margin);
    BarLayout l;
    l.bar = {s.x, s.y, s.w, kBarHeightDip};
    float inset = std::min(geometry::kCorner * std::min(s.w, s.h) * 0.55f, s.w * 0.12f);
    float left = s.x + inset;
    float right = s.x + s.w - inset;
    float size = 30.0f;
    float gap = 2.0f;
    float needed = button_count * (size + gap);
    if (needed > right - left) {
        size = std::max(18.0f, (right - left) / static_cast<float>(button_count) - gap);
        needed = button_count * (size + gap);
    }
    float x = right - needed + gap;
    float y = s.y + (kBarHeightDip - size) * 0.5f + 2.0f;
    for (size_t i = 0; i < button_count; ++i) {
        l.buttons.push_back({x, y, size, size});
        x += size + gap;
    }
    l.title = {left, s.y + 2.0f, std::max(0.0f, right - needed - left - 6.0f), kBarHeightDip - 2.0f};
    return l;
}

}  // namespace pm
