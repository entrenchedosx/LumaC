/* Phase 20 parallel command recording integration test.
 * Four CPU threads record real secondary graphics work into independent
 * command pools.  The lists are batch-executed in one offscreen pass and
 * pixel readback proves that the recorded draws reached the GPU.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <lumac/lumac.h>

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

typedef struct worker_job {
    lc_command_encoder *encoder;
    lc_render_target *target;
    lc_render_pass_desc pass;
    lc_pipeline *pipeline;
    lc_command_list *list;
    uint32_t draws;
    lc_result result;
    uint64_t begin_tick;
    uint64_t end_tick;
} worker_job;

typedef struct allocator_job {
    lc_device *device;
    int ok;
} allocator_job;

static int g_passed;
static int g_failed;
#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

static int load_spv(const char *name, void **out, size_t *out_size) {
    char path[512];
    FILE *file;
    long size;
    void *data;

    if (snprintf(path, sizeof(path), "%s/%s", LC_TRIANGLE_SPV_DIR,
                 name) < 0) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data); fclose(file); return 0;
    }
    fclose(file);
    *out = data; *out_size = (size_t)size;
    return 1;
}

static int make_shader(lc_device *device, lc_shader_stage stage,
                       const char *name, lc_shader **out) {
    lc_shader_desc desc;
    void *code = NULL;
    size_t size = 0;
    lc_result result;

    if (!load_spv(name, &code, &size)) return 0;
    memset(&desc, 0, sizeof(desc));
    desc.stage = stage; desc.code = code; desc.code_size = size;
    result = lc_shader_create(device, &desc, out);
    free(code);
    return result == LC_SUCCESS;
}

static void record_job(worker_job *job) {
    uint32_t i;

    job->begin_tick = lc_clock_now();
    job->result = lc_command_list_begin(job->encoder, job->target,
                                        &job->pass);
    if (job->result == LC_SUCCESS)
        job->result = lc_encoder_bind_pipeline(job->encoder, job->pipeline);
    for (i = 0; i < job->draws && job->result == LC_SUCCESS; i++)
        job->result = lc_encoder_draw(job->encoder, 3, 0);
    if (job->result == LC_SUCCESS)
        job->result = lc_command_encoder_finish(job->encoder, &job->list);
    job->end_tick = lc_clock_now();
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg) {
    record_job((worker_job *)arg);
    return 0;
}
static DWORD WINAPI allocator_main(LPVOID arg) {
    allocator_job *job = (allocator_job *)arg;
    uint32_t i;

    job->ok = 1;
    for (i = 0; i < 200; i++) {
        lc_buffer_desc desc;
        lc_buffer *buffer = NULL;
        void *mapped = NULL;

        memset(&desc, 0, sizeof(desc));
        desc.size = 4096 + (i & 7u) * 256u;
        desc.usage = LC_BUFFER_USAGE_UNIFORM;
        desc.memory = LC_MEMORY_CPU_TO_GPU;
        if (lc_buffer_create(job->device, &desc, &buffer) != LC_SUCCESS ||
            lc_buffer_map(buffer, &mapped) != LC_SUCCESS || mapped == NULL) {
            job->ok = 0;
        }
        lc_buffer_destroy(buffer);
        if (!job->ok) break;
    }
    return 0;
}
#else
static void *worker_main(void *arg) {
    record_job((worker_job *)arg);
    return NULL;
}
static void *allocator_main(void *arg) {
    allocator_job *job = (allocator_job *)arg;
    uint32_t i;

    job->ok = 1;
    for (i = 0; i < 200; i++) {
        lc_buffer_desc desc;
        lc_buffer *buffer = NULL;
        void *mapped = NULL;

        memset(&desc, 0, sizeof(desc));
        desc.size = 4096 + (i & 7u) * 256u;
        desc.usage = LC_BUFFER_USAGE_UNIFORM;
        desc.memory = LC_MEMORY_CPU_TO_GPU;
        if (lc_buffer_create(job->device, &desc, &buffer) != LC_SUCCESS ||
            lc_buffer_map(buffer, &mapped) != LC_SUCCESS || mapped == NULL) {
            job->ok = 0;
        }
        lc_buffer_destroy(buffer);
        if (!job->ok) break;
    }
    return NULL;
}
#endif

static double ms(uint64_t a, uint64_t b) {
    uint64_t f = lc_clock_frequency();
    return f ? (double)(b - a) * 1000.0 / (double)f : 0.0;
}

int main(void) {
    enum { THREADS = 4, DRAWS_PER_THREAD = 12500, W = 64, H = 64 };
    lc_device_desc ddesc;
    lc_device *device = NULL;
    lc_window_desc wdesc;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain_desc sdesc;
    lc_swapchain *swapchain = NULL;
    lc_image_desc idesc;
    lc_image *image = NULL;
    lc_image_view_desc vdesc;
    lc_image_view *view = NULL;
    lc_render_target_attachment rtatt;
    lc_render_target_create_desc rtdesc;
    lc_render_target *target = NULL;
    lc_shader *vs = NULL, *fs = NULL;
    lc_graphics_pipeline_desc pldesc;
    lc_pipeline *pipeline = NULL;
    lc_render_color_attachment color;
    lc_render_pass_desc pass;
    lc_render_swapchain_pass_desc swap_pass;
    lc_command_encoder_desc edesc;
    worker_job jobs[THREADS];
    allocator_job allocator_jobs[THREADS];
    lc_command_list *lists[THREADS];
    lc_command_encoder *primary = NULL;
    lc_image_readback_desc rb = {0};
    uint8_t pixels[W * H * 4];
    uint64_t record_start, record_end, submit_start, submit_end;
    double one_thread_ms = 0.0, two_thread_ms = 0.0;
    int setup = 1;
    uint32_t i;

#if defined(_WIN32)
    HANDLE threads[THREADS] = {0};
#else
    pthread_t threads[THREADS];
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) return 0;
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &device) != LC_SUCCESS) {
        printf("SKIP: no Vulkan device\n"); lc_shutdown(); return 0;
    }
    memset(allocator_jobs, 0, sizeof(allocator_jobs));
    for (i = 0; i < THREADS; i++) {
        allocator_jobs[i].device = device;
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, allocator_main,
                                  &allocator_jobs[i], 0, NULL);
#else
        pthread_create(&threads[i], NULL, allocator_main,
                       &allocator_jobs[i]);
#endif
    }
    for (i = 0; i < THREADS; i++) {
#if defined(_WIN32)
        if (threads[i] != NULL) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
            threads[i] = NULL;
        }
#else
        pthread_join(threads[i], NULL);
#endif
    }
    for (i = 0; i < THREADS; i++) {
        CHECK(allocator_jobs[i].ok,
              "concurrent shared allocator create/map/destroy");
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Phase 20 Recording";
    wdesc.width = 320; wdesc.height = 240;
    if (lc_window_create(&wdesc, &window) != LC_SUCCESS ||
        lc_surface_create(device, window, &surface) != LC_SUCCESS) setup = 0;
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 320; sdesc.height = 240; sdesc.vsync = 1;
    sdesc.max_frames_in_flight = 3;
    if (setup && lc_swapchain_create(device, surface, &sdesc, &swapchain) !=
                     LC_SUCCESS) setup = 0;
    if (!setup) {
        printf("SKIP: windowed Vulkan unavailable\n");
        lc_surface_destroy(surface); lc_device_destroy(device);
        lc_window_destroy(window); lc_shutdown(); return 0;
    }

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D; idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = W; idesc.height = H; idesc.depth = 1;
    idesc.mip_levels = 1; idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_TRANSFER_SRC | LC_IMAGE_USAGE_SAMPLED;
    idesc.samples = LC_SAMPLE_COUNT_1;
    CHECK(lc_image_create(device, &idesc, &image) == LC_SUCCESS,
          "offscreen image creates");
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1; vdesc.array_layer_count = 1;
    CHECK(lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS,
          "offscreen view creates");
    memset(&rtdesc, 0, sizeof(rtdesc));
    rtatt.view = view; rtdesc.color_attachments = &rtatt;
    rtdesc.color_attachment_count = 1; rtdesc.width = W; rtdesc.height = H;
    CHECK(lc_render_target_create(device, &rtdesc, &target) == LC_SUCCESS,
          "offscreen target creates");
    CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "triangle.vert.spv", &vs),
          "worker vertex shader creates");
    CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT, "triangle.frag.spv", &fs),
          "worker fragment shader creates");
    memset(&pldesc, 0, sizeof(pldesc));
    pldesc.vertex_shader = vs; pldesc.fragment_shader = fs;
    pldesc.render_target.color_attachment_count = 1;
    pldesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    pldesc.render_target.samples = LC_SAMPLE_COUNT_1;
    CHECK(lc_graphics_pipeline_create(device, &pldesc, &pipeline) == LC_SUCCESS,
          "worker pipeline creates");

    memset(&color, 0, sizeof(color));
    color.view = view; color.load_op = LC_LOAD_OP_CLEAR;
    color.store_op = LC_STORE_OP_STORE; color.clear_color[3] = 1.0f;
    memset(&pass, 0, sizeof(pass));
    pass.color_attachments = &color; pass.color_attachment_count = 1;
    pass.width = W; pass.height = H;
    edesc.queue = LC_QUEUE_GRAPHICS;
    memset(jobs, 0, sizeof(jobs));
    for (i = 0; i < THREADS; i++) {
        CHECK(lc_command_encoder_create(device, &edesc, &jobs[i].encoder) ==
                  LC_SUCCESS,
              "independent worker encoder creates");
        jobs[i].target = target; jobs[i].pass = pass;
        jobs[i].pipeline = pipeline; jobs[i].draws = DRAWS_PER_THREAD;
        jobs[i].result = LC_ERROR_UNKNOWN;
    }

    /* Same deterministic 50k-command workload at 1 and 2 threads.
     * These benchmark lists are not submitted; the four-thread run below
     * is both measured and pixel-verified on the GPU. */
    jobs[0].draws = THREADS * DRAWS_PER_THREAD;
    record_start = lc_clock_now();
    record_job(&jobs[0]);
    record_end = lc_clock_now();
    one_thread_ms = ms(record_start, record_end);
    CHECK(jobs[0].result == LC_SUCCESS, "one-thread 50000-draw recording");
    lc_command_list_destroy(jobs[0].list);
    jobs[0].list = NULL;
    for (i = 0; i < 2; i++) {
        jobs[i].draws = (THREADS * DRAWS_PER_THREAD) / 2;
        jobs[i].result = LC_ERROR_UNKNOWN;
    }
    record_start = lc_clock_now();
    for (i = 0; i < 2; i++) {
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, worker_main, &jobs[i], 0, NULL);
#else
        pthread_create(&threads[i], NULL, worker_main, &jobs[i]);
#endif
    }
    for (i = 0; i < 2; i++) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    record_end = lc_clock_now();
    two_thread_ms = ms(record_start, record_end);
    CHECK(jobs[0].result == LC_SUCCESS && jobs[1].result == LC_SUCCESS,
          "two-thread 50000-draw recording");
    for (i = 0; i < 2; i++) {
        lc_command_list_destroy(jobs[i].list);
        jobs[i].list = NULL;
    }
    for (i = 0; i < THREADS; i++) {
        jobs[i].draws = DRAWS_PER_THREAD;
        jobs[i].result = LC_ERROR_UNKNOWN;
    }

    record_start = lc_clock_now();
    for (i = 0; i < THREADS; i++) {
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, worker_main, &jobs[i], 0, NULL);
#else
        pthread_create(&threads[i], NULL, worker_main, &jobs[i]);
#endif
    }
    for (i = 0; i < THREADS; i++) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
        lists[i] = jobs[i].list;
    }
    record_end = lc_clock_now();
    for (i = 0; i < THREADS; i++)
        CHECK(jobs[i].result == LC_SUCCESS && lists[i] != NULL,
              "thread records pipeline bind plus 12500 real draws");

    submit_start = lc_clock_now();
    CHECK(lc_begin_frame(swapchain) == LC_SUCCESS &&
              lc_swapchain_get_encoder(swapchain, &primary) == LC_SUCCESS,
          "three-flight frame begins");
    CHECK(primary && lc_encoder_begin_render_pass(primary, &pass) == LC_SUCCESS,
          "primary offscreen pass begins");
    CHECK(primary && lc_encoder_execute_lists(primary, lists, THREADS) ==
                         LC_SUCCESS,
          "four command lists batch-execute in submission order");
    CHECK(primary && lc_encoder_end_render_pass(primary) == LC_SUCCESS,
          "primary offscreen pass ends");
    memset(&swap_pass, 0, sizeof(swap_pass));
    swap_pass.color_load_op = LC_LOAD_OP_CLEAR;
    swap_pass.color_store_op = LC_STORE_OP_STORE;
    swap_pass.clear_color[3] = 1.0f;
    swap_pass.depth_load_op = LC_LOAD_OP_CLEAR;
    swap_pass.depth_store_op = LC_STORE_OP_DONT_CARE;
    swap_pass.clear_depth = 1.0f;
    CHECK(primary && lc_encoder_begin_swapchain_pass(primary, swapchain,
                                                       &swap_pass) == LC_SUCCESS &&
              lc_encoder_end_render_pass(primary) == LC_SUCCESS,
          "swapchain pass prepares presentation");
    {
        lc_result end_result = lc_end_frame(swapchain);
        CHECK(end_result == LC_SUCCESS || end_result == LC_SUBOPTIMAL,
              "single graphics submission presents batch");
    }
    submit_end = lc_clock_now();
    /* Destruction immediately after submit must be non-blocking and must not
     * free VkCommandBuffers still owned by the GPU. */
    for (i = 0; i < THREADS; i++) {
        lc_command_list_destroy(lists[i]);
        lists[i] = NULL;
    }
    lc_device_wait_idle(device);

    memset(pixels, 0, sizeof(pixels));
    CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels), NULL) ==
              LC_SUCCESS,
          "worker output reads back");
    {
        uint8_t *center = &pixels[((H / 2) * W + W / 2) * 4];
        CHECK(center[3] > 240 && (center[0] + center[1] + center[2]) > 100,
              "pixel verification proves worker draws executed");
    }
    printf("[info] draws=%u threads=4 recording_ms=%.3f submit_ms=%.3f\n",
           THREADS * DRAWS_PER_THREAD, ms(record_start, record_end),
           ms(submit_start, submit_end));
    printf("[info] recording_1t_ms=%.3f recording_2t_ms=%.3f recording_4t_ms=%.3f\n",
           one_thread_ms, two_thread_ms, ms(record_start, record_end));
    for (i = 0; i < THREADS; i++) {
        printf("[info] worker_%u_ms=%.3f\n", i,
               ms(jobs[i].begin_tick, jobs[i].end_tick));
        lc_command_encoder_destroy(jobs[i].encoder);
    }

    lc_pipeline_destroy(pipeline); lc_shader_destroy(fs); lc_shader_destroy(vs);
    lc_render_target_destroy(target); lc_image_view_destroy(view);
    lc_image_destroy(image); lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface); lc_device_destroy(device);
    lc_window_destroy(window); lc_shutdown();
    printf("phase20 recording: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
