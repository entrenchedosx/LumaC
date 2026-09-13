/* Backend-neutral monotonic clock (Phase 18 profiling foundation).
 *
 * Win32: QueryPerformanceCounter (GetTickCount64 fallback).
 * POSIX: clock_gettime(CLOCK_MONOTONIC). Differences only; the epoch
 * is arbitrary. Callable from any thread; no LumaC init required.
 */

/* clock_gettime needs a POSIX feature macro under strict ISO C,
 * defined before ANY system header (including via lumac.h). */
#if !defined(_WIN32) && !defined(_WIN64) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "lumac/lumac.h"

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

uint64_t lc_clock_frequency(void) {
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER f;

    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        return (uint64_t)f.QuadPart;
    }
    return 10000ull; /* GetTickCount64 milliseconds */
#else
    return 1000000000ull; /* nanoseconds */
#endif
}

uint64_t lc_clock_now(void) {
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER t;

    if (QueryPerformanceCounter(&t)) {
        return (uint64_t)t.QuadPart;
    }
    /* Fallback scale is 10000 ticks/sec: milliseconds * 10. */
    return (uint64_t)GetTickCount64() * 10ull;
#else
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull +
               (uint64_t)ts.tv_nsec;
    }
    return 0;
#endif
}
