/* Phase 33 GUI public-ABI probes (headless): NULL-context refusal,
 * clip->scissor clamping, draw-walk budget caps.
 *
 * No window, no GPU, no renderer, no Dear ImGui linkage: these tests
 * drive ONLY the C ABI in luma_editor.h (leg_* pure helpers) plus a
 * NULL-context leg_context_create refusal probe. GPU-backed proofs
 * (font upload, blended draws, viewport composite) live in
 * test_editor_gui_gpu — including the positive leg_feed_event
 * mapping matrix (Phase 33V: this file never tested input mapping;
 * the old header claiming "input ownership defaults" overstated it).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <luma_editor/luma_editor.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 33 GUI headless tests...\n");

    /* NULL guards: pure helpers never crash; context create refuses. */
    {
        leg_context *ctx = (leg_context *)0x1;
        leg_draw_stats stats;
        lc_scissor_rect rect;
        lc_window_event ev;
        leg_frame_input in;

        memset(&stats, 0xAA, sizeof(stats));
        memset(&rect, 0xAA, sizeof(rect));
        memset(&ev, 0, sizeof(ev));
        memset(&in, 0, sizeof(in));
        TEST_CHECK(leg_context_create(NULL, NULL, NULL) ==
                        LED_ERROR_INVALID_ARGUMENT,
                    "context create NULL refuses");
        TEST_CHECK(leg_context_create(NULL, NULL, &ctx) ==
                        LED_ERROR_INVALID_ARGUMENT,
                    "context create NULL session refuses");
        leg_context_destroy(NULL);
        TEST_CHECK(leg_set_ini_path(NULL, NULL) ==
                        LED_ERROR_INVALID_ARGUMENT,
                    "ini path NULL refuses");
        TEST_CHECK(leg_frame_begin(NULL, NULL, NULL) ==
                        LED_ERROR_INVALID_ARGUMENT,
                    "frame begin NULL refuses");
        TEST_CHECK(leg_frame_end(NULL) == LED_ERROR_INVALID_ARGUMENT,
                    "frame end NULL refuses");
        leg_draw_get_stats(NULL, &stats);
        TEST_CHECK(stats.cmd_lists == 0 && stats.draw_cmds == 0 &&
                        stats.vertices == 0 && stats.indices == 0,
                    "draw stats NULL zeros");
        TEST_CHECK(!leg_feed_event(NULL, &ev), "feed NULL 0");
        TEST_CHECK(!leg_feed_event(NULL, NULL), "feed NULL NULL 0");
        TEST_CHECK(!leg_wants_keyboard(NULL), "wants kb NULL 0");
        TEST_CHECK(!leg_wants_mouse(NULL), "wants mouse NULL 0");
        TEST_CHECK(leg_clip_to_scissor(0, 0, 10, 10, 800, 600, NULL),
                    "clip NULL-out probe still reports nonempty");
    }

    /* Clip -> scissor: full/partial/empty/out-of-range/NaN. */
    {
        lc_scissor_rect r;

        memset(&r, 0, sizeof(r));
        /* Full pass rect passes through exactly. */
        TEST_CHECK(leg_clip_to_scissor(0, 0, 800, 600, 800, 600, &r),
                    "clip full 1");
        TEST_CHECK(r.offset_x == 0 && r.offset_y == 0 &&
                        r.width == 800 && r.height == 600,
                    "clip full exact");
        /* Partial overlap clamps (floor mins, ceil maxes). */
        memset(&r, 0, sizeof(r));
        TEST_CHECK(leg_clip_to_scissor(10.4f, 20.6f, 100.2f, 120.9f,
                                       800, 600, &r),
                    "clip partial 1");
        TEST_CHECK(r.offset_x == 10 && r.offset_y == 20 &&
                        r.width == 91 && r.height == 101,
                    "clip partial floor/ceil");
        /* Negative mins clamp to zero. */
        memset(&r, 0, sizeof(r));
        TEST_CHECK(leg_clip_to_scissor(-50, -40, 100, 100, 800, 600,
                                       &r),
                    "clip negative 1");
        TEST_CHECK(r.offset_x == 0 && r.offset_y == 0 &&
                        r.width == 100 && r.height == 100,
                    "clip negative clamped");
        /* Overhanging maxes clamp to the pass extent. */
        memset(&r, 0, sizeof(r));
        TEST_CHECK(leg_clip_to_scissor(700, 500, 2000, 2000, 800, 600,
                                       &r),
                    "clip overhang 1");
        TEST_CHECK(r.offset_x == 700 && r.offset_y == 500 &&
                        r.width == 100 && r.height == 100,
                    "clip overhang clamped");
        /* Fully outside -> empty (0, zeroed out). */
        memset(&r, 0xAA, sizeof(r));
        TEST_CHECK(!leg_clip_to_scissor(900, 700, 1000, 800, 800, 600,
                                        &r),
                    "clip outside 0");
        TEST_CHECK(r.offset_x == 0 && r.offset_y == 0 &&
                        r.width == 0 && r.height == 0,
                    "clip outside zeroed");
        /* Inverted rect -> empty. */
        TEST_CHECK(!leg_clip_to_scissor(100, 100, 50, 50, 800, 600,
                                        &r),
                    "clip inverted 0");
        /* Zero pass extent -> empty. */
        TEST_CHECK(!leg_clip_to_scissor(0, 0, 10, 10, 0, 600, &r),
                    "clip zero-w 0");
        TEST_CHECK(!leg_clip_to_scissor(0, 0, 10, 10, 800, 0, &r),
                    "clip zero-h 0");
        /* NaN/Inf clamp to empty, never crash. Bit-built (MSVC CRT
         * strtof("nan") is fine, but keep the probe independent of
         * libc parsing too). */
        {
            float nanf = 0.0f;
            float inff = 0.0f;
            uint32_t nbits = 0x7FC00000u;
            uint32_t ibits = 0x7F800000u;

            memcpy(&nanf, &nbits, sizeof(nanf));
            memcpy(&inff, &ibits, sizeof(inff));

            TEST_CHECK(!leg_clip_to_scissor(nanf, 0, 10, 10, 800, 600,
                                            &r),
                        "clip NaN 0");
            TEST_CHECK(!leg_clip_to_scissor(0, 0, inff, 10, 800, 600,
                                            &r),
                        "clip Inf 0");
        }
    }

    /* Draw budget: ImDrawVert = pos(8) + uv(8) + col(4) = 20 bytes;
     * indices 16-bit = 2 bytes. Caps at 64 MiB per buffer. */
    {
        uint64_t vb = 0;
        uint64_t ib = 0;
        int ov = 0;
        uint64_t est = 0;

        est = leg_draw_budget(100, 300, &vb, &ib, &ov);
        TEST_CHECK(est == 2000, "budget 100v -> 2000B");
        TEST_CHECK(vb == 2000 && ib == 600 && ov == 0,
                    "budget 100v/300i exact");
        est = leg_draw_budget(0, 0, &vb, &ib, &ov);
        TEST_CHECK(est == 0 && vb == 0 && ib == 0 && ov == 0,
                    "budget empty zero");
        /* Counting query (NULL outs) still returns the estimate. */
        est = leg_draw_budget(10, 30, NULL, NULL, NULL);
        TEST_CHECK(est == 200, "budget counting query");
        /* Overflow caps: 64 MiB per buffer. */
        est = leg_draw_budget(100000000u, 0, &vb, &ib, &ov);
        TEST_CHECK(ov != 0, "budget vertex overflow flagged");
        TEST_CHECK(vb == (64ull * 1024ull * 1024ull),
                    "budget vertex capped 64MiB");
        (void)est;
        ov = 0;
        ib = 0;
        leg_draw_budget(0, 100000000u, NULL, &ib, &ov);
        TEST_CHECK(ov != 0 && ib == (64ull * 1024ull * 1024ull),
                    "budget index capped 64MiB");
    }

    printf("gui headless: %d passed, %d failed\n", g_passed,
            g_failed);
    return (g_failed == 0) ? 0 : 1;
}
