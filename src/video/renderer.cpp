#include "video/renderer.h"

#include "app/log.h"
#include "present_ps.h"
#include "present_vs.h"
#include "ui/phone_geometry.h"

namespace pm {
namespace {

struct alignas(16) PresentParams {
    float viewport[2];
    float corner_radius;
    float pad0;
    float screen_rect[4];
    float bezel_color[4];
    float bg_top[4];
    float bg_bottom[4];
};

constexpr float kBezelColor[4] = {0.043f, 0.043f, 0.051f, 1};
constexpr float kIdleTop[4] = {0.075f, 0.082f, 0.110f, 1};
constexpr float kIdleBottom[4] = {0.024f, 0.027f, 0.039f, 1};

unsigned hex(HRESULT hr) { return static_cast<unsigned>(hr); }

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
    tearing_ = SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
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
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
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

    device_->CreateVertexShader(g_present_vs, sizeof(g_present_vs), nullptr, &vs_);
    device_->CreatePixelShader(g_present_ps, sizeof(g_present_ps), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(PresentParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&bd, nullptr, &params_);

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    if (FAILED(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_)) ||
        FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_))) {
        log::error("D2D device creation failed");
        return false;
    }
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.92f), &title_brush_);
    d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.55f), &detail_brush_);
    return vs_ && ps_ && params_;
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
    dpi_ = dpi;
    if (d2d_target_) {
        release_targets();
        create_targets();
    }
}

void Renderer::set_idle_text(std::wstring title, std::wstring detail) {
    title_ = std::move(title);
    detail_ = std::move(detail);
}

void Renderer::render() {
    if (!swapchain_ || !rtv_) return;
    WaitForSingleObjectEx(frame_wait_, 100, TRUE);

    geometry::Rect screen =
        geometry::screen_rect(static_cast<float>(width_), static_cast<float>(height_), screen_aspect_);
    PresentParams p{};
    p.viewport[0] = static_cast<float>(width_);
    p.viewport[1] = static_cast<float>(height_);
    p.corner_radius = geometry::kCorner * screen.w;
    p.screen_rect[0] = screen.x;
    p.screen_rect[1] = screen.y;
    p.screen_rect[2] = screen.x + screen.w;
    p.screen_rect[3] = screen.y + screen.h;
    std::copy_n(kBezelColor, 4, p.bezel_color);
    std::copy_n(kIdleTop, 4, p.bg_top);
    std::copy_n(kIdleBottom, 4, p.bg_bottom);
    ctx_->UpdateSubresource(params_.Get(), 0, nullptr, &p, 0, 0);

    D3D11_VIEWPORT vp{0, 0, p.viewport[0], p.viewport[1], 0, 1};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
    ctx_->Draw(3, 0);

    draw_idle_text();

    HRESULT hr = swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log::error("Device lost: 0x{:08X}", hex(device_->GetDeviceRemovedReason()));
        handle_device_lost();
    }
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

void Renderer::handle_device_lost() {
    release_targets();
    if (frame_wait_) {
        CloseHandle(frame_wait_);
        frame_wait_ = nullptr;
    }
    swapchain_.Reset();
    title_brush_.Reset();
    detail_brush_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
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
