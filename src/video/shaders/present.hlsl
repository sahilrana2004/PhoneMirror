cbuffer Params : register(b0) {
    float2 viewport;
    float corner_radius;
    float pad0;
    float4 screen_rect;  // x0, y0, x1, y1 in pixels
    float4 bezel_color;
    float4 bg_top;
    float4 bg_bottom;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float rounded_box(float2 p, float2 half_size, float r) {
    float2 q = abs(p) - half_size + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}

float4 ps_main(VSOut i) : SV_Target {
    float2 px = i.uv * viewport;
    float2 size = screen_rect.zw - screen_rect.xy;
    float d = rounded_box(px - (screen_rect.xy + size * 0.5), size * 0.5, corner_radius);
    float coverage = saturate(0.5 - d);
    float2 suv = (px - screen_rect.xy) / size;
    float3 screen = lerp(bg_top.rgb, bg_bottom.rgb, saturate(suv.y));
    return float4(lerp(bezel_color.rgb, screen, coverage), 1);
}
