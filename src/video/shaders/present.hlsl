cbuffer Params : register(b0) {
    float2 viewport;
    float corner_radius;
    float has_video;
    float4 screen_rect;  // x0, y0, x1, y1 in pixels
    float4 bezel_color;
    float4 bg_top;
    float4 bg_bottom;
    float4 yuv_r;  // rgb channel = dot(row.xyz, yuv) + row.w
    float4 yuv_g;
    float4 yuv_b;
    float2 uv_scale;  // decoded size / texture size (decoder surfaces are padded)
    float2 pad1;
};

Texture2D<float> tex_y : register(t0);
Texture2D<float2> tex_uv : register(t1);
SamplerState samp : register(s0);

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
    float2 suv = saturate((px - screen_rect.xy) / size);
    float3 screen;
    if (has_video > 0.5) {
        float2 tuv = suv * uv_scale;
        float3 yuv = float3(tex_y.SampleLevel(samp, tuv, 0), tex_uv.SampleLevel(samp, tuv, 0));
        screen = saturate(float3(dot(yuv_r.xyz, yuv) + yuv_r.w, dot(yuv_g.xyz, yuv) + yuv_g.w,
                                 dot(yuv_b.xyz, yuv) + yuv_b.w));
    } else {
        screen = lerp(bg_top.rgb, bg_bottom.rgb, suv.y);
    }
    return float4(lerp(bezel_color.rgb, screen, coverage), 1);
}
