#include "app/log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace pm::log {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;

}  // namespace

void init() {
    PWSTR appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) return;
    std::filesystem::path dir = std::filesystem::path(appdata) / L"PhoneMirror";
    CoTaskMemFree(appdata);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_file = _wfsopen((dir / L"phonemirror.log").c_str(), L"w", _SH_DENYWR);
}

void write(std::string_view level, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {} {}\n", t.wHour, t.wMinute, t.wSecond,
                                   t.wMilliseconds, GetCurrentThreadId(), level, message);
    std::lock_guard lock(g_mutex);
    OutputDebugStringA(line.c_str());
    if (g_file) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
    }
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace pm::log
