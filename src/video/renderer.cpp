#include "video/renderer.h"

#include <d3d10.h>

#include <cstring>

#include "app/log.h"
#include "present_ps.h"
#include "present_vs.h"
#include "ui/phone_geometry.h"

namespace pm {
namespace {

struct alignas(16) PresentParams {
    float viewport[2];
    float corner_radius;
    float has_video;
    float screen_rect[4];
    float bezel_color[4];
    float bg_top[4];
    float bg_bottom[4];
    float yuv[3][4];
    float uv_scale[2];
    float pad1[2];
};

constexpr float kBezelColor[4] = {0.043f, 0.043f, 0.051f, 1};
constexpr float kIdleTop[4] = {0.075f, 0.082f, 0.110f, 1};
constexpr float kIdleBottom[4] = {0.024f, 0.027f, 0.039f, 1};

unsigned hex(HRESULT hr) { return static_cast<unsigned>(hr); }

// Rows of the Y'CbCr -> R'G'B' affine transform for normalized texture values.
void yuv_matrix(bool full_range, bool bt601, float out[3][4]) {
    const float ys = full_range ? 1.0f : 255.0f / 219.0f;
    const float cs = full_range ? 1.0f : 255.0f / 224.0f;
    const float y0 = full_range ? 0.0f : 16.0f / 255.0f;
    const float c0 = 128.0f / 255.0f;
    const float rv = bt601 ? 1.402f : 1.5748f;
    const float gu = bt601 ? -0.344136f : -0.187324f;
    const float gv = bt601 ? -0.714136f : -0.468124f;
    const float bu = bt601 ? 1.772f : 1.8556f;
    const float r[4] = {ys, 0, cs * rv, -(ys * y0 + cs * rv * c0)};
    const float g[4] = {ys, cs * gu, cs * gv, -(ys * y0 + cs * (gu + gv) * c0)};
    const float b[4] = {ys, cs * bu, 0, -(ys * y0 + cs * bu * c0)};
    std::copy_n(r, 4, out[0]);
    std::copy_n(g, 4, out[1]);
    std::copy_n(b, 4, out[2]);
}

}  // namespace

Renderer::~Renderer() {
    if (frame_wait_) CloseHandle(frame_wait_);
}

bool Renderer::init(HWND hwnd, UINT dpi) {
    hwnd_ = hwnd;
    dpi_ = dpi;
    screen_aspect_ = geometry::kDefaultAspect;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max<LONG>(rc.right - rc.left, 1);
    height_ = std::max<LONG>(rc.bottom - rc.top, 1);

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) {
        log::error("CreateDXGIFactory2 failed");
        return false;
    }
    BOOL allow_tearing = FALSE;
    tearing_ = tearing_allowed_ &&
               SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                                       sizeof(allow_tearing))) &&
               allow_tearing;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), nullptr,
                                 reinterpret_cast<void**>(d2d_factory_.GetAddressOf()))) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        log::error("Direct2D / DirectWrite init failed");
        return false;
    }
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-us", &title_format_);
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &detail_format_);
    dwrite_->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &stats_format_);
    // Icon font: Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets (same code points) on Windows 10.
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(&fonts);
    UINT32 index = 0;
    BOOL fluent = FALSE;
    if (fonts) fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &fluent);
    dwrite_->CreateTextFormat(fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &glyph_format_);
    glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (IDWriteTextFormat* f : {title_format_.Get(), detail_format_.Get()}) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    return create_device() && create_swapchain() && create_targets();
}

bool Renderer::create_device() {
    // Render on the GPU that scans out the window's monitor; a cross-adapter present costs an extra copy.
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!chosen) chosen = adapter;
        bool owns_monitor = false;
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            output->GetDesc(&od);
            if (od.Monitor == monitor) owns_monitor = true;
        }
        if (owns_monitor) {
            chosen = adapter;
            break;
        }
    }
    if (!chosen) {
        log::error("No hardware adapter found");
        return false;
    }
    DXGI_ADAPTER_DESC1 desc;
    chosen->GetDesc1(&desc);
    log::info("GPU: {}", log::utf8(desc.Description));

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = E_FAIL;
#ifndef NDEBUG
    hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
#endif
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        log::error("D3D11CreateDevice failed: 0x{:08X}", hex(hr));
        return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    hevc_ = false;
    ComPtr<ID3D11VideoDevice> video;
    if (SUCCEEDED(device_.As(&video))) {
        for (UINT i = 0, n = video->GetVideoDecoderProfileCount(); i < n; ++i) {
            GUID profile;
            if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)) && profile == D3D11_DECODER_PROFILE_HEVC_VLD_MAIN) {
                hevc_ = true;
            }
        }
    }
    log::info("Hardware HEVC decode: {}", hevc_ ? "yes" : "no");

    device_->CreateVertexShader(g_present_vs, sizeof(g_present_vs), nullptr, &vs_);
    device_->CreatePixelShader(g_present_ps, sizeof(g_present_ps), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(PresentParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&bd, nullptr, &params_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    if (FAILED(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_)) ||
        FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_))) {
        log::error("D2D device creation failed");
        return false;
    }
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.92f), &title_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.55f), &detail_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.62f), &overlay_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.72f, 1.0f, 1.0f), &accent_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.23f, 1.0f), &red_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &hover_brush_);
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0, 0, 0.72f)}, {1.0f, D2D1::ColorF(0, 0, 0, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    d2d_ctx_->CreateGradientStopCollection(stops, 2, &collection);
    d2d_ctx_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
                                        collection.Get(), &bar_brush_);
    return vs_ && ps_ && params_ && sampler_ && bar_brush_;
}

bool Renderer::create_swapchain() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapchain_flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing_) swapchain_flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    desc.Flags = swapchain_flags_;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        log::error("CreateSwapChainForHwnd failed: 0x{:08X}", hex(hr));
        return false;
    }
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swapchain_);
    swapchain_->SetMaximumFrameLatency(1);
    if (frame_wait_) CloseHandle(frame_wait_);
    frame_wait_ = swapchain_->GetFrameLatencyWaitableObject();
    log::info("Swapchain {}x{} flip-discard, tearing {}", width_, height_, tearing_ ? "on" : "off");
    return true;
}

bool Renderer::create_targets() {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_);

    ComPtr<IDXGISurface> surface;
    back_buffer.As(&surface);
    float dpi = static_cast<float>(dpi_);
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &d2d_target_))) return false;
    d2d_ctx_->SetTarget(d2d_target_.Get());
    d2d_ctx_->SetDpi(dpi, dpi);
    return true;
}

void Renderer::release_targets() {
    if (d2d_ctx_) d2d_ctx_->SetTarget(nullptr);
    d2d_target_.Reset();
    rtv_.Reset();
    if (ctx_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
    }
}

void Renderer::resize() {
    std::lock_guard lock(lock_);
    if (!swapchain_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    UINT w = rc.right - rc.left;
    UINT h = rc.bottom - rc.top;
    if (w == 0 || h == 0 || (w == width_ && h == height_)) return;
    width_ = w;
    height_ = h;
    release_targets();
    HRESULT hr = swapchain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapchain_flags_);
    if (FAILED(hr)) {
        log::error("ResizeBuffers failed: 0x{:08X}", hex(hr));
        handle_device_lost();
        return;
    }
    create_targets();
}

void Renderer::set_dpi(UINT dpi) {
    std::lock_guard lock(lock_);
    dpi_ = dpi;
    if (d2d_target_) {
        release_targets();
        create_targets();
    }
}

void Renderer::set_screen_aspect(float aspect) {
    std::lock_guard lock(lock_);
    screen_aspect_ = aspect;
}

void Renderer::set_idle_text(std::wstring title, std::wstring detail) {
    std::lock_guard lock(lock_);
    title_ = std::move(title);
    detail_ = std::move(detail);
}

void Renderer::wait_for_swapchain() {
    if (frame_wait_) WaitForSingleObjectEx(frame_wait_, 100, TRUE);
}

void Renderer::render() {
    wait_for_swapchain();
    std::lock_guard lock(lock_);
    draw_and_present();
}

void Renderer::present_frame(const VideoFrame& frame) {
    wait_for_swapchain();
    std::lock_guard lock(lock_);
    frame_ = frame;
    has_frame_ = views_for(frame) != nullptr;
    draw_and_present();
}

void Renderer::clear_frame() {
    std::lock_guard lock(lock_);
    has_frame_ = false;
    frame_ = {};
    views_.clear();
    views_texture_.Reset();
    if (ctx_) {
        ID3D11ShaderResourceView* none[2] = {};
        ctx_->PSSetShaderResources(0, 2, none);
    }
}

const Renderer::SliceViews* Renderer::views_for(const VideoFrame& frame) {
    if (views_texture_.Get() != frame.texture) {
        views_.clear();
        views_texture_ = frame.texture;
    }
    for (const SliceViews& v : views_) {
        if (v.slice == frame.slice) return &v;
    }
    D3D11_TEXTURE2D_DESC desc;
    frame.texture->GetDesc(&desc);
    bool ten_bit = desc.Format == DXGI_FORMAT_P010;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.FirstArraySlice = frame.slice;
    sv.Texture2DArray.ArraySize = 1;
    SliceViews v{frame.slice, nullptr, nullptr};
    sv.Format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    HRESULT hr = device_->CreateShaderResourceView(frame.texture, &sv, &v.y);
    sv.Format = ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(frame.texture, &sv, &v.uv);
    if (FAILED(hr)) {
        log::error("Cannot sample decoder surface (format {}, bind 0x{:X}): 0x{:08X}", static_cast<int>(desc.Format),
                   desc.BindFlags, hex(hr));
        return nullptr;
    }
    views_.push_back(std::move(v));
    return &views_.back();
}

float Renderer::content_aspect() {
    std::lock_guard lock(lock_);
    return has_frame_ ? static_cast<float>(frame_.width) / static_cast<float>(frame_.height) : screen_aspect_;
}

void Renderer::draw_and_present() {
    if (!swapchain_ || !rtv_) return;

    float aspect = has_frame_ ? static_cast<float>(frame_.width) / static_cast<float>(frame_.height) : screen_aspect_;
    geometry::Rect screen = geometry::screen_rect(static_cast<float>(width_), static_cast<float>(height_), aspect);
    const float rect[4] = {screen.x, screen.y, screen.x + screen.w, screen.y + screen.h};
    const SliceViews* views = has_frame_ ? views_for(frame_) : nullptr;
    draw_quad(rtv_.Get(), static_cast<float>(width_), static_cast<float>(height_), rect,
              geometry::kCorner * std::min(screen.w, screen.h), views);

    if (!views) draw_idle_text();
    draw_hover_bar();
    draw_overlay();
    draw_stats();

    HRESULT hr = swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log::error("Device lost: 0x{:08X}", hex(device_->GetDeviceRemovedReason()));
        handle_device_lost();
    }
}

bool Renderer::capture_frame(std::vector<uint8_t>& bgra, UINT& width, UINT& height) {
    std::lock_guard lock(lock_);
    const SliceViews* views = has_frame_ ? views_for(frame_) : nullptr;
    if (!views) return false;
    width = frame_.width;
    height = frame_.height;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(device_->CreateRenderTargetView(target.Get(), nullptr, &rtv))) {
        return false;
    }
    const float rect[4] = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    draw_quad(rtv.Get(), static_cast<float>(width), static_cast<float>(height), rect, 0.0f, views);

    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return false;
    ctx_->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    bgra.resize(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(&bgra[static_cast<size_t>(y) * width * 4], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    static_cast<size_t>(width) * 4);
    }
    ctx_->Unmap(staging.Get(), 0);
    if (rtv_) ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    return true;
}

// Full-target pass: rounded phone screen (video or idle gradient) inside the bezel colour.
void Renderer::draw_quad(ID3D11RenderTargetView* target, float vw, float vh, const float rect[4], float radius,
                         const SliceViews* views) {
    PresentParams p{};
    p.viewport[0] = vw;
    p.viewport[1] = vh;
    p.corner_radius = radius;
    std::copy_n(rect, 4, p.screen_rect);
    std::copy_n(kBezelColor, 4, p.bezel_color);
    std::copy_n(kIdleTop, 4, p.bg_top);
    std::copy_n(kIdleBottom, 4, p.bg_bottom);

    if (views) {
        D3D11_TEXTURE2D_DESC desc;
        frame_.texture->GetDesc(&desc);
        p.has_video = 1;
        yuv_matrix(frame_.full_range, frame_.bt601, p.yuv);
        p.uv_scale[0] = static_cast<float>(frame_.width) / static_cast<float>(desc.Width);
        p.uv_scale[1] = static_cast<float>(frame_.height) / static_cast<float>(desc.Height);
        ID3D11ShaderResourceView* srvs[2] = {views->y.Get(), views->uv.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
    }
    ctx_->UpdateSubresource(params_.Get(), 0, nullptr, &p, 0, 0);

    D3D11_VIEWPORT vp{0, 0, vw, vh, 0, 1};
    ctx_->OMSetRenderTargets(1, &target, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->Draw(3, 0);
}

void Renderer::set_hover_bar(HoverBarState state) {
    std::lock_guard lock(lock_);
    bar_ = std::move(state);
}

void Renderer::draw_hover_bar() {
    if (bar_.opacity <= 0.01f && !bar_.recording) return;
    float scale = 96.0f / static_cast<float>(dpi_);
    float aspect = has_frame_ ? static_cast<float>(frame_.width) / static_cast<float>(frame_.height) : screen_aspect_;
    BarLayout l = hover_bar_layout(width_ * scale, height_ * scale, aspect, bar_.buttons.size());
    float a = bar_.opacity;

    d2d_ctx_->BeginDraw();
    if (a > 0.01f) {
        bar_brush_->SetOpacity(a);
        bar_brush_->SetStartPoint(D2D1::Point2F(0, l.bar.y));
        bar_brush_->SetEndPoint(D2D1::Point2F(0, l.bar.y + l.bar.h + 16.0f));
        d2d_ctx_->FillRectangle(D2D1::RectF(l.bar.x, l.bar.y, l.bar.x + l.bar.w, l.bar.y + l.bar.h + 16.0f), bar_brush_.Get());

        title_brush_->SetOpacity(0.92f * a);
        if (l.title.w > 24.0f && !bar_.title.empty()) {
            d2d_ctx_->DrawText(bar_.title.c_str(), static_cast<UINT32>(bar_.title.size()), detail_format_.Get(),
                               D2D1::RectF(l.title.x, l.title.y + 11.0f, l.title.x + l.title.w, l.title.y + l.title.h),
                               title_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        for (size_t i = 0; i < bar_.buttons.size(); ++i) {
            const BarButton& b = bar_.buttons[i];
            const geometry::Rect& r = l.buttons[i];
            D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(r.x + r.w / 2, r.y + r.h / 2), r.w / 2, r.h / 2);
            if (static_cast<int>(i) == bar_.hovered) {
                hover_brush_->SetOpacity(0.18f * a);
                d2d_ctx_->FillEllipse(circle, hover_brush_.Get());
            }
            ID2D1SolidColorBrush* brush = title_brush_.Get();
            if (b.action == BarAction::Record && b.active) {
                brush = red_brush_.Get();
            } else if (b.active) {
                brush = accent_brush_.Get();
            }
            brush->SetOpacity(a);
            const wchar_t glyph[2] = {b.glyph, 0};
            d2d_ctx_->DrawText(glyph, 1, glyph_format_.Get(), D2D1::RectF(r.x, r.y + r.h * 0.22f, r.x + r.w, r.y + r.h),
                               brush);
            brush->SetOpacity(1.0f);
        }
        title_brush_->SetOpacity(1.0f);
    }
    if (bar_.recording && !bar_.record_time.empty()) {
        // Recording badge stays visible when the bar is hidden.
        std::wstring badge = L"● " + bar_.record_time;
        float cx = l.bar.x + l.bar.w / 2;
        float y = l.bar.y + (a > 0.01f ? l.bar.h + 4.0f : 10.0f);
        D2D1_RECT_F pill = D2D1::RectF(cx - 38.0f, y, cx + 38.0f, y + 22.0f);
        d2d_ctx_->FillRoundedRectangle(D2D1::RoundedRect(pill, 11.0f, 11.0f), overlay_brush_.Get());
        d2d_ctx_->DrawText(badge.c_str(), static_cast<UINT32>(badge.size()), detail_format_.Get(),
                           D2D1::RectF(pill.left, pill.top + 2.0f, pill.right, pill.bottom), red_brush_.Get());
    }
    d2d_ctx_->EndDraw();
}

void Renderer::draw_idle_text() {
    if (title_.empty()) return;
    float scale = 96.0f / static_cast<float>(dpi_);
    geometry::Rect s =
        geometry::screen_rect(static_cast<float>(width_), static_cast<float>(height_), screen_aspect_);
    float left = (s.x + s.w * 0.1f) * scale;
    float right = (s.x + s.w * 0.9f) * scale;
    float mid = (s.y + s.h * 0.45f) * scale;

    d2d_ctx_->BeginDraw();
    d2d_ctx_->DrawText(title_.c_str(), static_cast<UINT32>(title_.size()), title_format_.Get(),
                       D2D1::RectF(left, mid - 26, right, mid), title_brush_.Get());
    d2d_ctx_->DrawText(detail_.c_str(), static_cast<UINT32>(detail_.size()), detail_format_.Get(),
                       D2D1::RectF(left, mid + 4, right, mid + 120), detail_brush_.Get());
    d2d_ctx_->EndDraw();
}

void Renderer::set_overlay(std::wstring text) {
    std::lock_guard lock(lock_);
    overlay_ = std::move(text);
}

void Renderer::draw_overlay() {
    if (overlay_.empty()) return;
    float scale = 96.0f / static_cast<float>(dpi_);
    float aspect = has_frame_ ? static_cast<float>(frame_.width) / static_cast<float>(frame_.height) : screen_aspect_;
    geometry::Rect s = geometry::screen_rect(static_cast<float>(width_), static_cast<float>(height_), aspect);
    ComPtr<IDWriteTextLayout> layout;
    dwrite_->CreateTextLayout(overlay_.c_str(), static_cast<UINT32>(overlay_.size()), detail_format_.Get(),
                              s.w * 0.9f * scale, 200.0f, &layout);
    if (!layout) return;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    float pad_x = 14.0f;
    float pad_y = 7.0f;
    float cx = (s.x + s.w * 0.5f) * scale;
    float bottom = (s.y + s.h) * scale - 28.0f;
    D2D1_RECT_F pill = D2D1::RectF(cx - m.width * 0.5f - pad_x, bottom - m.height - 2 * pad_y, cx + m.width * 0.5f + pad_x,
                                   bottom);
    d2d_ctx_->BeginDraw();
    float r = (pill.bottom - pill.top) * 0.5f;
    d2d_ctx_->FillRoundedRectangle(D2D1::RoundedRect(pill, r, r), overlay_brush_.Get());
    d2d_ctx_->DrawTextLayout(D2D1::Point2F(pill.left + pad_x - m.left, pill.top + pad_y), layout.Get(), title_brush_.Get());
    d2d_ctx_->EndDraw();
}

void Renderer::set_stats(std::wstring text) {
    std::lock_guard lock(lock_);
    stats_ = std::move(text);
}

void Renderer::set_stats_visible(bool visible) { stats_visible_ = visible; }

void Renderer::draw_stats() {
    if (!stats_visible_ || stats_.empty()) return;
    float scale = 96.0f / static_cast<float>(dpi_);
    float aspect = has_frame_ ? static_cast<float>(frame_.width) / static_cast<float>(frame_.height) : screen_aspect_;
    geometry::Rect s = geometry::screen_rect(static_cast<float>(width_), static_cast<float>(height_), aspect);
    ComPtr<IDWriteTextLayout> layout;
    dwrite_->CreateTextLayout(stats_.c_str(), static_cast<UINT32>(stats_.size()), stats_format_.Get(),
                              s.w * 0.92f * scale, 200.0f, &layout);
    if (!layout) return;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    float x = (s.x + s.w * 0.04f) * scale;
    float y = (s.y + s.w * 0.04f) * scale + 40.0f;  // below the hover bar / status bar area
    D2D1_RECT_F box = D2D1::RectF(x, y, x + m.width + 16.0f, y + m.height + 12.0f);
    d2d_ctx_->BeginDraw();
    d2d_ctx_->FillRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), overlay_brush_.Get());
    d2d_ctx_->DrawTextLayout(D2D1::Point2F(x + 8.0f, y + 6.0f), layout.Get(), title_brush_.Get());
    d2d_ctx_->EndDraw();
}

void Renderer::handle_device_lost() {
    // The decoder's surfaces die with the device; the next keyframe after a codec reset restores video.
    has_frame_ = false;
    views_.clear();
    views_texture_.Reset();
    release_targets();
    if (frame_wait_) {
        CloseHandle(frame_wait_);
        frame_wait_ = nullptr;
    }
    swapchain_.Reset();
    title_brush_.Reset();
    detail_brush_.Reset();
    overlay_brush_.Reset();
    accent_brush_.Reset();
    red_brush_.Reset();
    hover_brush_.Reset();
    bar_brush_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
    sampler_.Reset();
    params_.Reset();
    ps_.Reset();
    vs_.Reset();
    ctx_.Reset();
    device_.Reset();
    if (create_device() && create_swapchain() && create_targets()) {
        log::info("Renderer recovered from device loss");
    } else {
        log::error("Renderer recovery failed");
    }
}

}  // namespace pm
