// The subset of pthreads used by the vendored UxPlay sources, on native Win32 primitives
// (SRW locks and condition variables: no kernel transition on uncontended paths).
#pragma once

#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void pthread_attr_t;
typedef void pthread_mutexattr_t;
typedef void pthread_condattr_t;

typedef struct {
    void* (*fn)(void*);
    void* arg;
} pm_pthread_start_t;

static unsigned __stdcall pm_pthread_trampoline(void* p) {
    pm_pthread_start_t start = *(pm_pthread_start_t*)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

static __inline int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    (void)attr;
    pm_pthread_start_t* start = (pm_pthread_start_t*)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, pm_pthread_trampoline, start, 0, NULL);
    if (!h) {
        free(start);
        return -1;
    }
    *thread = (HANDLE)h;
    return 0;
}

static __inline int pthread_join(pthread_t thread, void** retval) {
    (void)retval;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static __inline int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr) {
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t* m) {
    AcquireSRWLockExclusive(m);
    return 0;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t* m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t* m) {
    (void)m;
    return 0;
}

static __inline int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr) {
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_signal(pthread_cond_t* c) {
    WakeConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_broadcast(pthread_cond_t* c) {
    WakeAllConditionVariable(c);
    return 0;
}
static __inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
    return 0;
}
// abstime is CLOCK_REALTIME, as produced by clock_gettime() in posix_compat.h.
static __inline int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abstime) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    long long ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    if (!SleepConditionVariableSRW(c, m, (DWORD)ms, 0)) return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    return 0;
}
static __inline int pthread_cond_destroy(pthread_cond_t* c) {
    (void)c;
    return 0;
}
