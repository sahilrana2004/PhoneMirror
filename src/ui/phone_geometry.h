#pragma once

#include <algorithm>

namespace pm::geometry {

// Bezel thickness and display corner radius, both as a fraction of the phone screen width.
inline constexpr float kBezel = 0.012f;
inline constexpr float kCorner = 0.14f;

// iPhone 15 / 16 / 17 family native portrait resolution; replaced by the stream size once connected.
inline constexpr float kDefaultAspect = 1179.0f / 2556.0f;

struct Rect {
    float x, y, w, h;
};

// Client-area aspect (w / h) for a screen of the given aspect surrounded by the bezel.
inline float client_aspect(float screen_aspect) {
    return (1.0f + 2.0f * kBezel) / (1.0f / screen_aspect + 2.0f * kBezel);
}

// Largest screen rect of the given aspect that fits the client while keeping a bezel on every side.
inline Rect screen_rect(float client_w, float client_h, float screen_aspect) {
    float w = std::min(client_w / (1.0f + 2.0f * kBezel), client_h / (1.0f / screen_aspect + 2.0f * kBezel));
    float h = w / screen_aspect;
    return {(client_w - w) * 0.5f, (client_h - h) * 0.5f, w, h};
}

}  // namespace pm::geometry
