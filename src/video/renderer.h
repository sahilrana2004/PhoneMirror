#pragma once

#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace pm {

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool init(HWND hwnd, UINT dpi);
    void resize();
    void render();

    void set_dpi(UINT dpi);
    void set_screen_aspect(float aspect) { screen_aspect_ = aspect; }
    void set_idle_text(std::wstring title, std::wstring detail);

private:
    bool create_device();
    bool create_swapchain();
    bool create_targets();
    void release_targets();
    void draw_idle_text();
    void handle_device_lost();

    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT dpi_ = 96;
    float screen_aspect_ = 0;
    bool tearing_ = false;
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

    ComPtr<ID2D1Factory3> d2d_factory_;
    ComPtr<ID2D1Device2> d2d_device_;
    ComPtr<ID2D1DeviceContext2> d2d_ctx_;
    ComPtr<ID2D1Bitmap1> d2d_target_;
    ComPtr<ID2D1SolidColorBrush> title_brush_;
    ComPtr<ID2D1SolidColorBrush> detail_brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> title_format_;
    ComPtr<IDWriteTextFormat> detail_format_;

    std::wstring title_;
    std::wstring detail_;
};

}  // namespace pm
