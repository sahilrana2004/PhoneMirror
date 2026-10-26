// Minimal <unistd.h> for the vendored UxPlay sources on MSVC.
#pragma once

#include <windows.h>

static __inline int usleep(unsigned long usec) {
    Sleep((DWORD)((usec + 999) / 1000));
    return 0;
}
