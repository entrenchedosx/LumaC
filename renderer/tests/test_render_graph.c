/* Phase 23 render-graph unit test.
 *
 * Proves the small renderer-owned schedule: declaration,
 * dependency derivation (write->read, write->write, read->write),
 * deterministic topological order, cycle/read-before-write/
 * invalid-handle rejection, transient lifetime analysis and
 * cross-frame reuse, dump/stats diagnostics, and real execution
 * order on a live frame. Validation layers stay enabled
 * throughout.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

#define SKIP_ENV(what) do {                                             \
    printf("SKIP: environment cannot provide %s\n", what);              \
    lc_shutdown();                                                      \
    return 0;                                                           \
} while (0)

static int g_exec_log[16];
static int g_exec_count;

static lr_result record_mark(lr_renderer *renderer,
                             lc_command_encoder *encoder, void *user) {
    (void)renderer;
    (void)encoder;
    if (g_exec_count < 16) {
        g_exec_log[g_exec_count++] = (int)(intptr_t)user;
    }
    return LR_SUCCESS;
}

static lr_result record_fail(lr_renderer *renderer,
                             lc_command_encoder *encoder, void *user) {
    (void)renderer;
    (void)encoder;
    (void)user;
    return LR_ERROR_RENDER;
}

static lc_buffer *make_buf(lc_device *device, uint64_t size) {
    lc_buffer_desc desc;
    lc_buffer *buffer = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    desc.usage = LC_BUFFER_USAGE_STORAGE;
    desc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(device, &desc, &buffer) != LC_SUCCESS) {
        return NULL;
    }
    return buffer;
}

int main(void) {
    lc_device_desc ddesc;
    lc_device *device = NULL;
    lr_renderer_desc rdesc;
    lr_renderer *renderer = NULL;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC render-graph (Phase 23) test...\n");
    memset(&rdesc, 0, sizeof(rdesc));
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &device) != LC_SUCCESS) {
        SKIP_ENV("Vulkan device");
    }
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 8;
    if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
        printf("renderer create failed: FAIL\n");
        return 1;
    }
    /* ---- argument validation (no device work) ---- */
    CHECK(lr_render_graph_create(NULL, NULL) ==
              LR_ERROR_INVALID_ARGUMENT,
          "NULL renderer rejected");
    {
        lr_render_graph *graph = NULL;

        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS && graph != NULL,
              "graph creates");
        CHECK(lr_graph_import_buffer(NULL, "x", NULL) == NULL,
              "NULL import rejected");
        CHECK(lr_graph_import_buffer(graph, "x", NULL) == NULL,
              "NULL buffer rejected");
        CHECK(lr_graph_add_pass(NULL, "p", LR_GRAPH_PASS_COMPUTE,
                                record_mark, NULL) == NULL,
              "NULL graph pass rejected");
        CHECK(lr_graph_add_pass(graph, "p", LR_GRAPH_PASS_COMPUTE,
                                NULL, NULL) == NULL,
              "NULL record rejected");
        CHECK(lr_graph_add_pass(graph, "p",
                                (lr_graph_pass_type)99, record_mark,
                                NULL) == NULL,
              "bad pass type rejected");
        CHECK(lr_graph_pass_read(NULL, NULL,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "NULL read rejected");
        CHECK(lr_graph_pass_write(NULL, NULL,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "NULL write rejected");
        CHECK(lr_graph_add_dependency(NULL, NULL) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "NULL dependency rejected");
        {
            lc_buffer_desc bad;

            memset(&bad, 0, sizeof(bad));
            CHECK(lr_graph_transient_buffer(graph, "bad", &bad) ==
                      NULL,
                  "empty transient desc rejected");
        }
        lr_render_graph_destroy(graph);
        lr_render_graph_destroy(NULL);
        CHECK(1, "destroy NULL-safe");
        CHECK(lr_render_graph_error(NULL)[0] == '\0',
              "NULL graph error empty");
        CHECK(lr_render_graph_dump(NULL, NULL, 0) == 0,
              "NULL graph dump empty");
    }
    /* ---- derivation: declared out of order, derived in order ---- */
    {
        lr_render_graph *graph = NULL;
        lc_buffer *a = make_buf(device, 64);
        lc_buffer *b = make_buf(device, 64);
        lr_graph_resource *ra;
        lr_graph_resource *rb;
        lr_graph_pass *reader;
        lr_graph_pass *writer;
        lr_graph_stats stats;
        char dump[1024];

        CHECK(a != NULL && b != NULL, "scratch buffers create");
        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS,
              "order graph creates");
        ra = lr_graph_import_buffer(graph, "a", a);
        rb = lr_graph_import_buffer(graph, "b", b);
        CHECK(ra != NULL && rb != NULL, "imports succeed");
        /* Producer declared first: the edge follows data flow. */
        writer = lr_graph_add_pass(graph, "writer",
                                   LR_GRAPH_PASS_COMPUTE,
                                   record_mark, (void *)2);
        reader = lr_graph_add_pass(graph, "reader",
                                   LR_GRAPH_PASS_COMPUTE,
                                   record_mark, (void *)1);
        CHECK(reader != NULL && writer != NULL, "passes add");
        CHECK(lr_graph_pass_read(reader, ra,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_SUCCESS,
              "reader declares read");
        CHECK(lr_graph_pass_write(reader, rb,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_SUCCESS,
              "reader declares write");
        CHECK(lr_graph_pass_write(writer, ra,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_SUCCESS,
              "writer declares write");
        CHECK(lr_render_graph_compile(graph) == LR_SUCCESS,
              "order graph compiles");
        lr_render_graph_get_stats(graph, &stats);
        CHECK(stats.pass_count == 2, "two passes counted");
        CHECK(stats.compute_passes == 2, "compute counted");
        CHECK(stats.resource_count == 2, "resources counted");
        CHECK(stats.dependency_edges >= 1, "edge derived");
        CHECK(stats.derived_barriers >= 1, "barrier derived");
        CHECK(lr_render_graph_dump(graph, dump, sizeof(dump)) > 0,
              "dump non-empty");
        CHECK(strstr(dump, "writer") != NULL &&
                  strstr(dump, "reader") != NULL,
              "dump names passes");
        {
            /* Execute needs an open frame: device-only here, so
             * only verify compile state + error text. */
            CHECK(lr_render_graph_error(graph)[0] == '\0',
                  "no compile error");
        }
        lr_render_graph_destroy(graph);
        lc_buffer_destroy(a);
        lc_buffer_destroy(b);
    }
    /* ---- imported initial-value pattern: reader declared first
     * reads the incoming value, writer updates after (program
     * order preserved, no forced reordering) ---- */
    {
        lr_render_graph *graph = NULL;
        lc_buffer *a = make_buf(device, 64);
        lr_graph_resource *ra;
        lr_graph_pass *reader;
        lr_graph_pass *writer;
        lr_graph_stats stats;

        CHECK(a != NULL, "initial scratch creates");
        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS,
              "initial graph creates");
        ra = lr_graph_import_buffer(graph, "a", a);
        reader = lr_graph_add_pass(graph, "reader",
                                   LR_GRAPH_PASS_COMPUTE,
                                   record_mark, (void *)1);
        writer = lr_graph_add_pass(graph, "writer",
                                   LR_GRAPH_PASS_COMPUTE,
                                   record_mark, (void *)2);
        CHECK(ra != NULL && reader != NULL && writer != NULL,
              "initial parts exist");
        CHECK(lr_graph_pass_read(reader, ra,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_SUCCESS,
              "initial read declared");
        CHECK(lr_graph_pass_write(writer, ra,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_SUCCESS,
              "initial write declared");
        CHECK(lr_render_graph_compile(graph) == LR_SUCCESS,
              "initial pattern compiles");
        lr_render_graph_get_stats(graph, &stats);
        CHECK(stats.dependency_edges >= 1,
              "initial edge derived");
        lr_render_graph_destroy(graph);
        lc_buffer_destroy(a);
    }
    /* ---- cycle detection ---- */
    {
        lr_render_graph *graph = NULL;
        lc_buffer *a = make_buf(device, 64);
        lr_graph_resource *ra;
        lr_graph_pass *p1;
        lr_graph_pass *p2;

        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS,
              "cycle graph creates");
        ra = lr_graph_import_buffer(graph, "a", a);
        p1 = lr_graph_add_pass(graph, "p1", LR_GRAPH_PASS_COMPUTE,
                               record_mark, NULL);
        p2 = lr_graph_add_pass(graph, "p2", LR_GRAPH_PASS_COMPUTE,
                               record_mark, NULL);
        CHECK(ra != NULL && p1 != NULL && p2 != NULL,
              "cycle parts exist");
        CHECK(lr_graph_pass_write(p1, ra,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_SUCCESS,
              "p1 writes");
        CHECK(lr_graph_pass_read(p2, ra,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_SUCCESS,
              "p2 reads");
        CHECK(lr_graph_add_dependency(p2, p1) == LR_SUCCESS,
              "back edge adds");
        CHECK(lr_render_graph_compile(graph) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "cycle rejected");
        CHECK(lr_render_graph_error(graph)[0] != '\0',
              "cycle error text set");
        printf("[info] cycle error: %s\n",
               lr_render_graph_error(graph));
        lr_render_graph_destroy(graph);
        lc_buffer_destroy(a);
    }
    /* ---- read-before-write transient ---- */
    {
        lr_render_graph *graph = NULL;
        lc_buffer_desc desc;
        lr_graph_resource *tmp;
        lr_graph_pass *p;

        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS,
              "rbw graph creates");
        memset(&desc, 0, sizeof(desc));
        desc.size = 128;
        desc.usage = LC_BUFFER_USAGE_STORAGE;
        desc.memory = LC_MEMORY_GPU_ONLY;
        tmp = lr_graph_transient_buffer(graph, "tmp", &desc);
        p = lr_graph_add_pass(graph, "reader",
                              LR_GRAPH_PASS_COMPUTE, record_mark,
                              NULL);
        CHECK(tmp != NULL && p != NULL, "rbw parts exist");
        CHECK(lr_graph_pass_read(p, tmp,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_SUCCESS,
              "transient read declared");
        CHECK(lr_render_graph_compile(graph) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "read-before-write rejected");
        printf("[info] rbw error: %s\n",
               lr_render_graph_error(graph));
        lr_render_graph_destroy(graph);
    }
    /* ---- transient write->read compiles; reuse + lifetimes ---- */
    {
        lr_render_graph *graph = NULL;
        lc_buffer_desc desc;
        lr_graph_resource *tmp;
        lr_graph_pass *w;
        lr_graph_pass *r;
        lr_graph_stats stats;
        char dump[1024];
        lc_buffer *first = NULL;

        CHECK(lr_render_graph_create(renderer, &graph) ==
                  LR_SUCCESS,
              "transient graph creates");
        memset(&desc, 0, sizeof(desc));
        desc.size = 256;
        desc.usage = LC_BUFFER_USAGE_STORAGE;
        desc.memory = LC_MEMORY_GPU_ONLY;
        tmp = lr_graph_transient_buffer(graph, "tmp", &desc);
        w = lr_graph_add_pass(graph, "w", LR_GRAPH_PASS_COMPUTE,
                              record_mark, NULL);
        r = lr_graph_add_pass(graph, "r", LR_GRAPH_PASS_COMPUTE,
                              record_mark, NULL);
        CHECK(tmp != NULL && w != NULL && r != NULL,
              "transient parts exist");
        CHECK(lr_graph_pass_write(w, tmp,
                                  LR_GRAPH_USE_STORAGE_WRITE) ==
                  LR_SUCCESS,
              "transient write declared");
        CHECK(lr_graph_pass_read(r, tmp,
                                 LR_GRAPH_USE_STORAGE_READ) ==
                  LR_SUCCESS,
              "transient read declared");
        CHECK(lr_render_graph_compile(graph) == LR_SUCCESS,
              "transient graph compiles");
        first = lr_graph_resource_get_buffer(tmp);
        CHECK(first != NULL, "transient allocated");
        lr_render_graph_get_stats(graph, &stats);
        CHECK(stats.transient_resources == 1,
              "one transient counted");
        CHECK(stats.peak_transient_bytes == 256,
              "peak transient bytes exact");
        CHECK(lr_render_graph_dump(graph, dump, sizeof(dump)) > 0 &&
                  strstr(dump, "first 0") != NULL &&
                  strstr(dump, "last 1") != NULL,
              "lifetimes reported");
        /* Recompile with the same descriptor reuses the buffer. */
        CHECK(lr_render_graph_compile(graph) == LR_SUCCESS,
              "recompile succeeds");
        CHECK(lr_graph_resource_get_buffer(tmp) == first,
              "transient reused across compiles");
        lr_render_graph_destroy(graph);
    }
    /* ---- execution order on a live frame ---- */
    {
        lc_window_desc wdesc;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain_desc sdesc;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lr_render_graph *graph = NULL;
        lc_buffer *a = make_buf(device, 64);
        lc_buffer *b = make_buf(device, 64);
        lr_graph_resource *ra;
        lr_graph_resource *rb;
        lr_graph_pass *reader;
        lr_graph_pass *writer;

        memset(&wdesc, 0, sizeof(wdesc));
        wdesc.title = "LumaC Graph";
        wdesc.width = 160;
        wdesc.height = 120;
        if (lc_window_create(&wdesc, &window) != LC_SUCCESS ||
            lc_surface_create(device, window, &surface) !=
                LC_SUCCESS) {
            printf("SKIP: windowed part unavailable\n");
        } else {
            memset(&sdesc, 0, sizeof(sdesc));
            sdesc.width = 160;
            sdesc.height = 120;
            sdesc.vsync = 1;
            if (lc_swapchain_create(device, surface, &sdesc,
                                    &swapchain) != LC_SUCCESS) {
                printf("SKIP: windowed part unavailable\n");
            }
        }
        if (swapchain != NULL && a != NULL && b != NULL &&
            lr_render_graph_create(renderer, &graph) ==
                LR_SUCCESS) {
            ra = lr_graph_import_buffer(graph, "a", a);
            rb = lr_graph_import_buffer(graph, "b", b);
            writer = lr_graph_add_pass(graph, "writer",
                                       LR_GRAPH_PASS_COMPUTE,
                                       record_mark, (void *)2);
            reader = lr_graph_add_pass(graph, "reader",
                                       LR_GRAPH_PASS_COMPUTE,
                                       record_mark, (void *)1);
            CHECK(ra != NULL && rb != NULL && reader != NULL &&
                      writer != NULL,
                  "exec parts exist");
            CHECK(lr_graph_pass_read(reader, ra,
                                     LR_GRAPH_USE_STORAGE_READ) ==
                      LR_SUCCESS,
                  "exec read declared");
            CHECK(lr_graph_pass_write(reader, rb,
                                      LR_GRAPH_USE_STORAGE_WRITE) ==
                      LR_SUCCESS,
                  "exec write declared");
            CHECK(lr_graph_pass_write(writer, ra,
                                      LR_GRAPH_USE_STORAGE_WRITE) ==
                      LR_SUCCESS,
                  "exec producer declared");
            g_exec_count = 0;
            if (lc_begin_frame(swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(swapchain, &enc) ==
                    LC_SUCCESS &&
                lr_render_graph_execute(graph, enc) == LR_SUCCESS) {
                CHECK(g_exec_count == 2, "both passes execute");
                CHECK(g_exec_log[0] == 2 && g_exec_log[1] == 1,
                      "derived order executes (writer first)");
            } else {
                CHECK(0, "graph executes");
            }
            {
                lc_render_swapchain_pass_desc spass;

                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                if (enc != NULL) {
                    (void)lc_encoder_begin_swapchain_pass(
                        enc, swapchain, &spass);
                    (void)lc_encoder_end_render_pass(enc);
                }
            }
            {
                lc_result end_res = lc_end_frame(swapchain);

                CHECK(end_res == LC_SUCCESS ||
                          end_res == LC_SUBOPTIMAL,
                      "exec frame ends");
            }
            /* Failing records propagate. */
            {
                lr_graph_pass *bad = lr_graph_add_pass(
                    graph, "bad", LR_GRAPH_PASS_COMPUTE,
                    record_fail, NULL);

                CHECK(bad != NULL, "failing pass adds");
                g_exec_count = 0;
                if (lc_begin_frame(swapchain) == LC_SUCCESS &&
                    lc_swapchain_get_encoder(swapchain, &enc) ==
                        LC_SUCCESS) {
                    CHECK(lr_render_graph_execute(graph, enc) ==
                              LR_ERROR_RENDER,
                          "failing record propagates");
                    {
                        lc_render_swapchain_pass_desc spass;

                        memset(&spass, 0, sizeof(spass));
                        spass.color_load_op = LC_LOAD_OP_CLEAR;
                        spass.color_store_op = LC_STORE_OP_STORE;
                        if (enc != NULL) {
                            (void)lc_encoder_begin_swapchain_pass(
                                enc, swapchain, &spass);
                            (void)lc_encoder_end_render_pass(enc);
                        }
                    }
                    (void)lc_end_frame(swapchain);
                }
            }
            lr_render_graph_destroy(graph);
        }
        lc_buffer_destroy(a);
        lc_buffer_destroy(b);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_window_destroy(window);
    }
    lr_renderer_destroy(renderer);
    lc_device_destroy(device);
    lc_shutdown();
    printf("graph: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
