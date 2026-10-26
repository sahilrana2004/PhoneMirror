#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <timeapi.h>

#include <cstdio>
#include <format>
#include <string_view>

#include "app/app.h"
#include "app/log.h"
#include "app/settings.h"
#include "version.h"

namespace {

// Unhandled crash -> %APPDATA%\PhoneMirror\crashes\PhoneMirror-<time>.dmp for post-mortem debugging.
LONG WINAPI write_crash_dump(EXCEPTION_POINTERS* info) {
    std::filesystem::path dir = pm::Settings::data_dir() / L"crashes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::filesystem::path path = dir / std::format(L"PhoneMirror-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", t.wYear, t.wMonth,
                                                   t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          &mei, nullptr, nullptr);
        CloseHandle(file);
        pm::log::error("Crashed (0x{:08X}); dump written to {}", info->ExceptionRecord->ExceptionCode,
                       pm::log::utf8(path.wstring()));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void print_to_parent_console(std::string_view text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    pm::LaunchOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"--version") {
            print_to_parent_console("PhoneMirror " PM_VERSION_STRING "\n");
            LocalFree(argv);
            return 0;
        }
        if (arg == L"--play" && i + 1 < argc) options.play_video = argv[++i];
        if (arg == L"--play-audio" && i + 1 < argc) options.play_audio = argv[++i];
    }
    LocalFree(argv);

    // One app per machine (it hosts every receiver slot): a second launch just surfaces the windows.
    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\PhoneMirror.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(pm::kAppWindowClass, nullptr)) PostMessageW(existing, pm::kMsgShowWindow, 0, 0);
        return 0;
    }

    SetUnhandledExceptionFilter(&write_crash_dump);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (screenshots), firewall/network COM APIs
    timeBeginPeriod(1);
    pm::log::init();
    pm::Settings::instance().load();

    int code;
    {
        pm::App app;
        code = app.run(instance, options);
    }
    timeEndPeriod(1);
    CoUninitialize();
    CloseHandle(instance_mutex);
    return code;
}
