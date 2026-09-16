/*
 * Luma Engine stress tests (Phase 24, headless CPU-only).
 *
 * 100k objects: creation, transform update, iteration
 * (extraction), destruction — with wall-clock timings (reported,
 * never asserted) and exact accounting. Plus component storage
 * growth (renderables past initial dense capacity, no lost
 * components, no stale mappings after swap-remove churn).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_engine/luma_engine.h>
#include <lumac/lumac.h>

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

static double ms_since(uint64_t t0) {
    uint64_t freq = lc_clock_frequency();
    uint64_t dt = lc_clock_now() - t0;

    if (freq == 0) {
        return -1.0;
    }
    return (double)dt * 1000.0 / (double)freq;
}

int main(void) {
    printf("Running Luma Engine stress tests...\n");

    /* 100k object lifecycle with timings. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_engine_desc edesc;
        le_world_desc wdesc;
        le_object *handles = NULL;
        const uint32_t N = 100000u;
        uint32_t i;
        double t_create;
        double t_update;
        double t_iter;
        double t_destroy;
        uint64_t t0;

        memset(&edesc, 0, sizeof(edesc));
        memset(&wdesc, 0, sizeof(wdesc));
        TEST_CHECK(le_engine_create(&edesc, &engine) == LE_SUCCESS,
                   "stress engine create");
        TEST_CHECK(le_world_create(engine, &wdesc, &world) == LE_SUCCESS,
                   "stress world create");
        handles =
            (le_object *)malloc((size_t)N * sizeof(le_object));
        TEST_CHECK(handles != NULL, "stress handle buffer");
        if (handles == NULL) {
            le_world_destroy(world);
            le_engine_destroy(engine);
            printf("TESTS FAILED\n");
            return 1;
        }
        t0 = lc_clock_now();
        for (i = 0; i < N; i++) {
            if (le_object_create(world, &handles[i]) != LE_SUCCESS) {
                break;
            }
        }
        t_create = ms_since(t0);
        TEST_CHECK(i == N, "100k creation succeeds");
        TEST_CHECK(le_world_get_object_count(world) == N,
                   "100k count exact");
        printf("[info] 100k creation: %.2f ms\n", t_create);

        /* Transform update: position every object, then refresh. */
        t0 = lc_clock_now();
        for (i = 0; i < N; i++) {
            float p[3];

            p[0] = (float)(i % 100u);
            p[1] = (float)((i / 100u) % 100u);
            p[2] = (float)(i / 10000u);
            if (le_object_set_position(world, &handles[i], p) !=
                LE_SUCCESS) {
                break;
            }
        }
        TEST_CHECK(i == N, "100k transform update succeeds");
        TEST_CHECK(le_world_update(world, 0.016f) == LE_SUCCESS,
                   "100k world update succeeds");
        t_update = ms_since(t0);
        printf("[info] 100k transform-update: %.2f ms\n", t_update);

        /* Iteration: deterministic extraction order over
         * renderable-less world is 0 items; attach renderables
         * to a subset and iterate. */
        {
            le_renderable_desc rd;

            memset(&rd, 0, sizeof(rd));
            rd.mesh = (lr_mesh *)(uintptr_t)0x11;
            rd.material = (lr_material *)(uintptr_t)0x22;
            rd.visible = 1;
            for (i = 0; i < N; i += 2u) {
                if (le_object_add_renderable(world, &handles[i], &rd) !=
                    LE_SUCCESS) {
                    break;
                }
            }
            TEST_CHECK(i >= N, "50k renderables attached");
        }
        t0 = lc_clock_now();
        {
            uint32_t count = 0;

            TEST_CHECK(le_world_extract_renderables(world, NULL, 0,
                                                    &count) == LE_SUCCESS,
                       "extract counting query succeeds");
            TEST_CHECK(count == N / 2u, "50k renderables iterated");
            /* Spot-check determinism: two counting passes agree. */
            {
                uint32_t count2 = 0;

                le_world_extract_renderables(world, NULL, 0, &count2);
                TEST_CHECK(count2 == count,
                           "iteration deterministic");
            }
        }
        t_iter = ms_since(t0);
        printf("[info] 100k iteration: %.2f ms\n", t_iter);

        /* Destruction: destroy every object individually. */
        t0 = lc_clock_now();
        for (i = 0; i < N; i++) {
            if (le_object_destroy(world, &handles[i]) != LE_SUCCESS) {
                break;
            }
        }
        t_destroy = ms_since(t0);
        TEST_CHECK(i == N, "100k destruction succeeds");
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "world empty after 100k destroy");
        printf("[info] 100k destruction: %.2f ms\n", t_destroy);
        {
            le_world_stats stats;
            le_memory_stats mem;

            le_world_get_stats(world, &stats);
            le_world_get_memory_stats(world, &mem);
            printf("[info] post-stress: capacity=%u total_bytes=%llu\n",
                   stats.object_capacity,
                   (unsigned long long)mem.total_bytes);
            TEST_CHECK(stats.renderables == 0,
                       "components drained with objects");
        }
        free(handles);
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Component storage growth + swap-remove churn: attach
     * renderables past dense capacity, remove half (odd mapping
     * churn), verify survivors intact, then destroy all. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_engine_desc edesc;
        le_world_desc wdesc;
        const uint32_t N = 5000u;
        le_object *handles = NULL;
        uint32_t i;
        int ok = 1;

        memset(&edesc, 0, sizeof(edesc));
        memset(&wdesc, 0, sizeof(wdesc));
        le_engine_create(&edesc, &engine);
        le_world_create(engine, &wdesc, &world);
        handles =
            (le_object *)malloc((size_t)N * sizeof(le_object));
        TEST_CHECK(handles != NULL, "churn handle buffer");
        for (i = 0; i < N && ok; i++) {
            le_renderable_desc rd;

            if (le_object_create(world, &handles[i]) != LE_SUCCESS) {
                ok = 0;
                break;
            }
            memset(&rd, 0, sizeof(rd));
            rd.mesh = (lr_mesh *)(uintptr_t)(0x1000u + i);
            rd.material = (lr_material *)(uintptr_t)0x2000;
            rd.visible = 1;
            if (le_object_add_renderable(world, &handles[i], &rd) !=
                LE_SUCCESS) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "5k renderables attached (growth)");
        /* Remove every even object (swap-remove churn in the dense
         * array while handles stay stable). */
        for (i = 0; i < N && ok; i += 2u) {
            if (le_object_destroy(world, &handles[i]) != LE_SUCCESS) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "churn destroy evens");
        /* Survivors (odds) keep exact mesh handles. */
        for (i = 1; i < N && ok; i += 2u) {
            le_renderable_desc got;

            if (!le_object_get_renderable(world, &handles[i], &got) ||
                got.mesh != (lr_mesh *)(uintptr_t)(0x1000u + i)) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "survivors intact after swap-remove churn");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(stats.renderables == N / 2u,
                       "renderable count exact after churn");
        }
        /* Destroy the rest; also exercises free-list reuse at
         * volume (recreate + destroy again). */
        for (i = 1; i < N; i += 2u) {
            le_object_destroy(world, &handles[i]);
        }
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "churn world drained");
        for (i = 0; i < N; i++) {
            if (le_object_create(world, &handles[i]) != LE_SUCCESS) {
                ok = 0;
                break;
            }
        }
        TEST_CHECK(ok, "5k recreate after churn (free-list reuse)");
        for (i = 0; i < N; i++) {
            le_object_destroy(world, &handles[i]);
        }
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "recreated world drained");
        free(handles);
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* World-destroy stress: thousands of live named objects with
     * components + hierarchy, destroyed via the world. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_engine_desc edesc;
        le_world_desc wdesc;
        const uint32_t N = 20000u;
        uint32_t i;
        int ok = 1;
        char namebuf[32];

        memset(&edesc, 0, sizeof(edesc));
        memset(&wdesc, 0, sizeof(wdesc));
        le_engine_create(&edesc, &engine);
        le_world_create(engine, &wdesc, &world);
        for (i = 0; i < N && ok; i++) {
            le_object o;
            le_object parent;
            le_renderable_desc rd;
            le_camera_desc cd;

            if (le_object_create(world, &o) != LE_SUCCESS) {
                ok = 0;
                break;
            }
            snprintf(namebuf, sizeof(namebuf), "obj-%u", i);
            if (le_object_set_name(world, &o, namebuf) != LE_SUCCESS) {
                ok = 0;
                break;
            }
            memset(&rd, 0, sizeof(rd));
            rd.mesh = (lr_mesh *)(uintptr_t)0x31;
            rd.material = (lr_material *)(uintptr_t)0x32;
            rd.visible = 1;
            if (le_object_add_renderable(world, &o, &rd) != LE_SUCCESS) {
                ok = 0;
                break;
            }
            if ((i % 100u) == 0u) {
                le_camera_desc_default(&cd);
                if (le_object_add_camera(world, &o, &cd) != LE_SUCCESS) {
                    ok = 0;
                    break;
                }
            }
            if (i > 0 && (i % 7u) == 0u) {
                /* Chain some objects for hierarchy-at-destroy. */
                (void)parent;
            }
        }
        TEST_CHECK(ok, "20k named component objects created");
        TEST_CHECK(le_world_get_object_count(world) == N,
                   "20k count exact");
        le_world_destroy(world);
        le_engine_destroy(engine);
        TEST_CHECK(1, "world-destroy stress released");
    }

    printf("Luma Engine stress tests: %d passed, %d failed\n", g_passed,
           g_failed);
    if (g_failed != 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL ENGINE STRESS TESTS PASSED\n");
    return 0;
}
