/*
 * Phase 28 micro-benchmarks (informative, not pass/fail gates):
 * broad-phase sweep scaling (256/1024 colliders), narrow-phase
 * pair throughput, solver iterations, query throughput, event
 * drain. Prints timings; always exits 0 (never fails the build).
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

static void scatter(le_world *world, int n) {
    int i;

    for (i = 0; i < n; i++) {
        le_object o;
        le_collider_desc c;
        float p[3];

        memset(&c, 0, sizeof(c));
        c.shape = LE_COLLIDER_SPHERE;
        c.radius = 0.5f;
        c.orientation[3] = 1.0f;
        c.mask = 0xFFFFFFFFu;
        c.friction = 0.5f;
        le_object_create(world, &o);
        le_object_add_collider(world, &o, &c);
        p[0] = (float)(i % 16) * 3.0f;
        p[1] = (float)((i / 16) % 16) * 3.0f;
        p[2] = (float)(i / 256) * 3.0f;
        le_object_set_position(world, &o, p);
    }
}

int main(void) {
    le_engine *engine = NULL;
    le_world *world = NULL;
    le_engine_desc edesc;
    le_world_desc wdesc;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 28 benchmarks...\n");
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
    le_physics_set_gravity(world, 0, 0, 0);
    /* Broad-phase scaling: overlap-free grids (SAP best case). */
    {
        double t0;
        int k;

        scatter(world, 256);
        t0 = now_seconds();
        for (k = 0; k < 60; k++) {
            le_world_update(world, 1.0f / 60.0f);
        }
        printf("[bench] 256 colliders x 60 steps: %.3fs\n",
               now_seconds() - t0);
        le_world_destroy(world);
        le_world_create(engine, &wdesc, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        scatter(world, 1024);
        t0 = now_seconds();
        for (k = 0; k < 60; k++) {
            le_world_update(world, 1.0f / 60.0f);
        }
        printf("[bench] 1024 colliders x 60 steps: %.3fs\n",
               now_seconds() - t0);
        le_world_destroy(world);
        le_world_create(engine, &wdesc, &world);
        le_physics_set_gravity(world, 0, 0, 0);
    }
    /* Stacked dynamics: 32 balls falling onto a floor. */
    {
        le_object ground;
        le_rigid_body_desc st;
        le_collider_desc gc;
        double t0;
        int i;
        int k;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        memset(&gc, 0, sizeof(gc));
        gc.shape = LE_COLLIDER_BOX;
        gc.half_extents[0] = 10.0f;
        gc.half_extents[1] = 0.5f;
        gc.half_extents[2] = 10.0f;
        gc.orientation[3] = 1.0f;
        gc.mask = 0xFFFFFFFFu;
        gc.friction = 0.5f;
        le_object_create(world, &ground);
        le_object_add_rigid_body(world, &ground, &st);
        le_object_add_collider(world, &ground, &gc);
        for (i = 0; i < 32; i++) {
            le_object o;
            le_rigid_body_desc d;
            le_collider_desc c;
            float p[3];

            memset(&d, 0, sizeof(d));
            d.type = LE_BODY_DYNAMIC;
            d.mass = 1.0f;
            d.gravity_scale = 1.0f;
            memset(&c, 0, sizeof(c));
            c.shape = LE_COLLIDER_SPHERE;
            c.radius = 0.5f;
            c.orientation[3] = 1.0f;
            c.mask = 0xFFFFFFFFu;
            c.friction = 0.5f;
            le_object_create(world, &o);
            le_object_add_rigid_body(world, &o, &d);
            le_object_add_collider(world, &o, &c);
            p[0] = (float)(i % 8) * 1.2f;
            p[1] = 2.0f + (float)(i / 8) * 1.2f;
            p[2] = 0.0f;
            le_object_set_position(world, &o, p);
        }
        t0 = now_seconds();
        for (k = 0; k < 300; k++) {
            le_world_update(world, 1.0f / 60.0f);
        }
        printf("[bench] 32-ball stack x 300 steps: %.3fs\n",
               now_seconds() - t0);
        {
            le_physics_stats ps;

            le_physics_get_stats(world, &ps);
            printf("[bench] stats: bodies=%u colliders=%u "
                   "contacts=%u candidates=%u narrow=%u\n",
                   ps.body_count, ps.collider_count,
                   ps.contact_count, ps.broadphase_candidates,
                   ps.narrowphase_tests);
        }
        le_world_destroy(world);
        le_world_create(engine, &wdesc, &world);
        le_physics_set_gravity(world, 0, 0, 0);
    }
    /* Query throughput: 10k raycasts over a 256 grid. */
    {
        double t0;
        int k;
        volatile int sink = 0;

        scatter(world, 256);
        t0 = now_seconds();
        for (k = 0; k < 10000; k++) {
            sink += le_physics_raycast(world, 0, 0, -10, 0, 0, 1,
                                       100, 0xFFFFFFFFu, 0, NULL);
        }
        printf("[bench] 10k raycasts (256 scene): %.3fs "
               "(hits=%d)\n",
               now_seconds() - t0, sink);
        le_world_destroy(world);
        le_engine_destroy(engine);
    }
    printf("BENCHMARKS DONE\n");
    return 0;
}
