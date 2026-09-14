/* Phase 22 clock-contract test (PART T, host-only).
 *
 * Pins the nanosecond contract without any GPU: monotonicity,
 * frequency identity, and a bounded sleep-delta sanity check with
 * generous bounds (loaded CI machines must not flake).
 */
#include <stdint.h>
#include <stdio.h>

#include <lumac/lumac.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

static void sleep_ms_50(void) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep(50);
#else
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    nanosleep(&ts, NULL);
#endif
}

int main(void) {
    uint64_t freq = lc_clock_frequency();
    uint64_t a = lc_clock_now();
    uint64_t b = lc_clock_now();
    uint64_t c;
    uint64_t d;

    CHECK(freq == 1000000000ull, "frequency is 1e9 (nanoseconds)");
    CHECK(b >= a, "clock is monotonic (back-to-back)");
    sleep_ms_50();
    c = lc_clock_now();
    CHECK(c >= b, "clock is monotonic (across sleep)");
    /* 50 ms sleep must read as tens of millions of ns: generous
     * bounds (20 ms .. 1000 ms) so loaded machines never flake,
     * while wrong units (ticks/us/ms) fail loudly. */
    CHECK(c - b >= 20ull * 1000000ull, "50ms sleep >= 20ms in ns");
    CHECK(c - b <= 1000ull * 1000000ull, "50ms sleep <= 1000ms in ns");
    d = lc_clock_now();
    CHECK(d >= c, "clock is monotonic (final)");
    printf("[info] freq=%llu delta_ns=%llu\n",
           (unsigned long long)freq, (unsigned long long)(c - b));
    printf("phase22 clock: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
