/* Backend-neutral monotonic clock (Phase 18 profiling foundation,
 * Phase 22 nanosecond contract).
 *
 * PUBLIC CONTRACT: lc_clock_now() returns NANOSECONDS on every
 * platform (monotonic, differences only; the epoch is arbitrary),
 * and lc_clock_frequency() returns 1,000,000,000 (ticks per
 * second, where one tick is one nanosecond). Timeout APIs take
 * nanoseconds and compare directly against lc_clock_now() with no
 * platform conversion at the call site.
 *
 * Win32: QueryPerformanceCounter scaled overflow-safely to ns
 * (GetTickCount64 fallback at millisecond granularity).
 * POSIX: clock_gettime(CLOCK_MONOTONIC). Callable from any thread;
 * no LumaC init required.
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
    /* Nanoseconds per second, by the contract above, on every
     * platform. Existing *1000/frequency converters keep working
     * unchanged (they now convert nanoseconds). */
    return 1000000000ull;
}

uint64_t lc_clock_now(void) {
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER t;
    LARGE_INTEGER f;

    if (QueryPerformanceCounter(&t) &&
        QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        uint64_t ticks = (uint64_t)t.QuadPart;
        uint64_t freq = (uint64_t)f.QuadPart;

        /* Overflow-safe ticks -> ns: split seconds and remainder.
         * (ticks % freq) * 1e9 cannot overflow for any realistic
         * QPC rate; absurd rates fall back to millisecond time. */
        if (freq > 0 && freq <= 1000000000000ull) {
            return (ticks / freq) * 1000000000ull +
                   ((ticks % freq) * 1000000000ull) / freq;
        }
    }
    /* Fallback: millisecond granularity, scaled to nanoseconds. */
    return (uint64_t)GetTickCount64() * 1000000ull;
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
