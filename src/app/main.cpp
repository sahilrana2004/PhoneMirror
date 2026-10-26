#include <windows.h>

#include "app/app.h"
#include "app/log.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // One receiver per machine: a second launch just surfaces the existing window.
    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\PhoneMirror.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(pm::kAppWindowClass, nullptr)) {
            PostMessageW(existing, pm::kMsgShowWindow, 0, 0);
        }
        return 0;
    }

    pm::log::init();
    pm::App app;
    int code = app.run(instance);
    CloseHandle(instance_mutex);
    return code;
}
