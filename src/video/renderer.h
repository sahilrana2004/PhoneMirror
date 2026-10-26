#pragma once

#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace pm {

using Microsoft::WRL::ComPtr;

// One decoded picture: a slice of the decoder's NV12/P010 texture array, sampled in place (zero-copy).
struct VideoFrame {
    ID3D11Texture2D* texture = nullptr;
    UINT slice = 0;
    UINT width = 0;
    UINT height = 0;
    bool full_range = false;
    bool bt601 = false;
};

// Owns the device and swapchain. Every device-context use (including the FFmpeg decoder, through
// device_lock()) is serialized on one recursive mutex; the frame-latency wait happens outside it so a
// thread waiting for the display never stalls the decoder.
class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool init(HWND hwnd, UINT dpi);
    void resize();
    void render();
    void present_frame(const VideoFrame& frame);
    void clear_frame();

    void set_dpi(UINT dpi);
    void set_screen_aspect(float aspect);
    void set_idle_text(std::wstring title, std::wstring detail);
    void set_overlay(std::wstring text);  // short hint pill over the screen; empty hides it

    ID3D11Device* device() const { return device_.Get(); }
    std::recursive_mutex& device_lock() { return lock_; }
    bool supports_hevc() const { return hevc_; }

private:
    struct SliceViews {
        UINT slice;
        ComPtr<ID3D11ShaderResourceView> y;
        ComPtr<ID3D11ShaderResourceView> uv;
    };

    bool create_device();
    bool create_swapchain();
    bool create_targets();
    void release_targets();
    void wait_for_swapchain();
    void draw_and_present();
    void draw_idle_text();
    void draw_overlay();
    void handle_device_lost();
    const SliceViews* views_for(const VideoFrame& frame);

    std::recursive_mutex lock_;
    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT dpi_ = 96;
    float screen_aspect_ = 0;
    bool tearing_ = false;
    bool hevc_ = false;
    UINT swapchain_flags_ = 0;
    HANDLE frame_wait_ = nullptr;

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> params_;
    ComPtr<ID3D11SamplerState> sampler_;

    VideoFrame frame_;
    bool has_frame_ = false;
    ComPtr<ID3D11Texture2D> views_texture_;
    std::vector<SliceViews> views_;

    ComPtr<ID2D1Factory3> d2d_factory_;
    ComPtr<ID2D1Device2> d2d_device_;
    ComPtr<ID2D1DeviceContext2> d2d_ctx_;
    ComPtr<ID2D1Bitmap1> d2d_target_;
    ComPtr<ID2D1SolidColorBrush> title_brush_;
    ComPtr<ID2D1SolidColorBrush> detail_brush_;
    ComPtr<ID2D1SolidColorBrush> overlay_brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> title_format_;
    ComPtr<IDWriteTextFormat> detail_format_;

    std::wstring title_;
    std::wstring detail_;
    std::wstring overlay_;
};

}  // namespace pm
