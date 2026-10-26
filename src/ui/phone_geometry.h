#pragma once

#include <algorithm>

namespace pm::geometry {

// Display corner radius as a fraction of the screen's shorter side (iPhone 15 / 16 / 17 family).
inline constexpr float kCorner = 0.14f;

// Windowed mode: the window is the phone screen plus a transparent margin that holds the soft drop shadow
// and the resize handles; nothing rectangular is visible. Fullscreen uses no margin.
inline constexpr float kShadowMarginDip = 20.0f;

// iPhone 17 Pro native portrait resolution; replaced by the stream size once connected.
inline constexpr float kDefaultAspect = 1206.0f / 2622.0f;

struct Rect {
    float x, y, w, h;
};

// Largest screen rect of the given aspect inside the client after the margin, centred.
inline Rect screen_rect(float client_w, float client_h, float screen_aspect, float margin) {
    float w = std::max(1.0f, std::min(client_w - 2.0f * margin, (client_h - 2.0f * margin) * screen_aspect));
    float h = w / screen_aspect;
    return {(client_w - w) * 0.5f, (client_h - h) * 0.5f, w, h};
}

// Window size that fits a screen of the given aspect exactly (aspect-locked resizing).
inline float client_height_for_width(float w, float screen_aspect, float margin) {
    return (w - 2.0f * margin) / screen_aspect + 2.0f * margin;
}
inline float client_width_for_height(float h, float screen_aspect, float margin) {
    return (h - 2.0f * margin) * screen_aspect + 2.0f * margin;
}

}  // namespace pm::geometry
