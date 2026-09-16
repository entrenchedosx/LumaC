/*
 * Phase 27 micro-benchmarks (informative, not pass/fail gates):
 * raw queries, 100/1000-action evaluation, frame overhead,
 * fixed-step overhead, Lua input binding observations.
 * Prints timings; always exits 0 (never fails the build).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <luma_engine/luma_engine.h>

static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(void) {
    le_engine *engine = NULL;
    le_world *world = NULL;
    le_engine_desc edesc;
    le_world_desc wdesc;
    char name[64];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 27 benchmarks...\n");
    memset(&edesc, 0, sizeof(edesc));
    if (le_engine_create(&edesc, &engine) != LE_SUCCESS) {
        printf("engine create failed\n");
        return 1;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
        printf("world create failed\n");
        le_engine_destroy(engine);
        return 1;
    }
    /* Raw query cost: 1M key_down queries. */
    {
        double t0 = now_seconds();
        volatile int sink = 0;
        int k;

        for (k = 0; k < 1000000; k++) {
            sink += le_input_key_down(engine, LE_KEY_W);
        }
        printf("[bench] 1M raw key_down: %.3fs (%s)\n",
               now_seconds() - t0,
               sink == 0 ? "idle-correct" : "UNEXPECTED");
    }
    /* 100 actions x 10k evaluations. */
    {
        le_input_action acts[100];
        le_input_binding b;
        double t0;
        volatile int sink = 0;
        int k;
        int r;

        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_SPACE;
        b.scale = 1.0f;
        for (i = 0; i < 100; i++) {
            snprintf(name, sizeof(name), "bench100_%d", i);
            if (le_input_create_action(engine, name, &acts[i]) !=
                LE_SUCCESS) {
                printf("action create failed at %d\n", i);
                le_world_destroy(world);
                le_engine_destroy(engine);
                return 1;
            }
            le_input_add_action_binding(engine, &acts[i], &b);
        }
        t0 = now_seconds();
        for (k = 0; k < 10000; k++) {
            for (r = 0; r < 100; r++) {
                sink += le_input_action_down(engine, &acts[r]);
            }
        }
        printf("[bench] 100 actions x 10k evals (1M queries): "
               "%.3fs\n",
               now_seconds() - t0);
        (void)sink;
    }
    /* 1000 actions x 1k evaluations. */
    {
        le_input_action acts[1000];
        le_input_binding b;
        double t0;
        volatile int sink = 0;
        int k;
        int r;

        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_SPACE;
        b.scale = 1.0f;
        for (i = 0; i < 1000; i++) {
            snprintf(name, sizeof(name), "bench1000_%d", i);
            if (le_input_create_action(engine, name, &acts[i]) !=
                LE_SUCCESS) {
                printf("action create failed at %d\n", i);
                le_world_destroy(world);
                le_engine_destroy(engine);
                return 1;
            }
            le_input_add_action_binding(engine, &acts[i], &b);
        }
        t0 = now_seconds();
        for (k = 0; k < 1000; k++) {
            for (r = 0; r < 1000; r++) {
                sink += le_input_action_down(engine, &acts[r]);
            }
        }
        printf("[bench] 1000 actions x 1k evals (1M queries): "
               "%.3fs\n",
               now_seconds() - t0);
        (void)sink;
    }
    /* Frame lifecycle overhead: 10k empty steps. */
    {
        double t0 = now_seconds();
        int k;

        for (k = 0; k < 10000; k++) {
            le_engine_step(engine, world, 0.016f);
        }
        printf("[bench] 10k empty engine steps: %.3fs\n",
               now_seconds() - t0);
    }
    /* Fixed-step overhead: 1k steps of 0.1s (6 fixed each max). */
    {
        double t0 = now_seconds();
        int k;

        for (k = 0; k < 1000; k++) {
            le_engine_step(engine, world, 0.1f);
        }
        printf("[bench] 1k x 0.1s steps (fixed schedule): %.3fs\n",
               now_seconds() - t0);
    }
    le_world_destroy(world);
    le_engine_destroy(engine);
    printf("BENCHMARKS DONE\n");
    return 0;
}
