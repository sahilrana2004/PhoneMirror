// Force-included into every vendored UxPlay C file: fills the few POSIX gaps MSVC's CRT leaves.
#pragma once

#ifdef _MSC_VER

#include <stdio.h>
#include <time.h>

// compat.h maps snprintf to _snprintf, which does not NUL-terminate on truncation; route it back to
// UCRT's conforming snprintf (stdio.h is already in, so its macro guard does not fire).
#define _snprintf snprintf

// Winsock takes TCP-level options (TCP_KEEPIDLE etc., ws2ipdef.h) at IPPROTO_TCP.
#define SOL_TCP IPPROTO_TCP

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
typedef int clockid_t;
static __inline int clock_gettime(clockid_t clock, struct timespec* ts) {
    (void)clock;
    return timespec_get(ts, TIME_UTC) == TIME_UTC ? 0 : -1;
}
#endif

#endif
