#include "record/screenshot.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <format>

namespace pm {

using Microsoft::WRL::ComPtr;

bool save_png(const std::filesystem::path& path, const std::vector<uint8_t>& bgra, UINT width, UINT height) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              format == GUID_WICPixelFormat32bppBGRA &&
              SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    return ok;
}

bool copy_image_to_clipboard(HWND owner, const std::vector<uint8_t>& bgra, UINT width, UINT height) {
    // CF_DIB: bottom-up 32-bit BITMAPINFOHEADER + pixels.
    size_t pixels = static_cast<size_t>(width) * height * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
    if (!mem) return false;
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(mem));
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    header->biHeight = static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    auto* dst = reinterpret_cast<uint8_t*>(header + 1);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(height - 1 - y) * width * 4, &bgra[static_cast<size_t>(y) * width * 4],
                    static_cast<size_t>(width) * 4);
    }
    GlobalUnlock(mem);
    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

std::filesystem::path media_folder(const KNOWNFOLDERID& folder) {
    PWSTR base = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_CREATE, nullptr, &base))) dir = std::filesystem::path(base) / L"PhoneMirror";
    CoTaskMemFree(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::wstring timestamped_stem(const wchar_t* prefix) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return std::format(L"{} {:04}-{:02}-{:02} {:02}.{:02}.{:02}", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                       t.wSecond);
}

}  // namespace pm
