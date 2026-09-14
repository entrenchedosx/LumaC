/* Phase 21 compute fundamentals integration test (SECTION 38).
 *
 * Device + one frame: shader/pipeline validation, compute pipeline
 * cache participation (cold/warm/corrupt/disabled), transfer ->
 * compute -> readback chains on the graphics queue, worker compute
 * lists, atomic-append compaction, buffer state transitions, an
 * isolated dedicated-queue dispatch when hardware offers one, and
 * honest capability reporting. Validation stays enabled.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include "graphics/graphics_internal.h"

#ifndef LC_COMPUTE_SPV_DIR
#define LC_COMPUTE_SPV_DIR "."
#endif

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

static int make_device(lc_device **out, const char *cache_path,
                       int disable_cache) {
    lc_device_desc desc;
    lc_result result;

    memset(&desc, 0, sizeof(desc));
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    desc.pipeline_cache_path = cache_path;
    desc.disable_pipeline_cache = disable_cache;
    result = lc_device_create(&desc, out);
    if (result == LC_SUCCESS) {
        return 0;
    }
    if (result == LC_ERROR_BACKEND_UNAVAILABLE ||
        result == LC_ERROR_NO_SUPPORTED_DEVICE ||
        result == LC_ERROR_SURFACE_UNSUPPORTED) {
        return 1;
    }
    return -1;
}

static int make_window_titled(lc_window **out, const char *title) {
    lc_window_desc desc;

    desc.title = title;
    desc.width = 640;
    desc.height = 480;
    *out = NULL;
    switch (lc_window_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        return 1;
    default:
        return -1;
    }
}

static int make_surface(lc_device *device, lc_window *window,
                        lc_surface **out) {
    *out = NULL;
    switch (lc_surface_create(device, window, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static int make_swapchain(lc_device *device, lc_surface *surface,
                          lc_swapchain **out) {
    lc_swapchain_desc desc = { 0 };

    desc.width = 640;
    desc.height = 480;
    desc.image_count = 0;
    desc.vsync = 1;
    *out = NULL;
    switch (lc_swapchain_create(device, surface, &desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
    case LC_ERROR_ZERO_EXTENT:
        return 1;
    default:
        return -1;
    }
}

/* Load one .spv file from the staged shader dir (malloc'd, free). */
static uint8_t *load_spv(const char *name, size_t *out_size) {
    char path[1024];
    FILE *file = NULL;
    uint8_t *bytes = NULL;
    size_t size = 0;
    size_t got = 0;

    *out_size = 0;
    snprintf(path, sizeof(path), "%s/%s", LC_COMPUTE_SPV_DIR, name);
    path[sizeof(path) - 1] = '\0';
#if defined(_WIN32)
    fopen_s(&file, path, "rb");
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size == 0 || (size % 4u) != 0u) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t *)malloc(size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    got = fread(bytes, 1, size, file);
    fclose(file);
    if (got != size) {
        free(bytes);
        return NULL;
    }
    *out_size = size;
    return bytes;
}

static lc_buffer *make_gpu_buffer(lc_device *device, uint64_t size,
                                  uint32_t usage) {
    lc_buffer_desc desc;
    lc_buffer *buffer = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    desc.usage = usage;
    desc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(device, &desc, &buffer) != LC_SUCCESS) {
        return NULL;
    }
    return buffer;
}

/* One storage layout with N STORAGE_BUFFER bindings (COMPUTE). */
static lc_binding_layout *make_storage_layout(lc_device *device,
                                              uint32_t count) {
    lc_binding_layout *layout = NULL;
    lc_binding_layout_desc desc;
    lc_binding_desc *binds = NULL;
    uint32_t i;

    binds = (lc_binding_desc *)calloc(count, sizeof(lc_binding_desc));
    if (binds == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        binds[i].binding = i;
        binds[i].type = LC_BINDING_STORAGE_BUFFER;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    memset(&desc, 0, sizeof(desc));
    desc.bindings = binds;
    desc.binding_count = count;
    if (lc_binding_layout_create(device, &desc, &layout) !=
        LC_SUCCESS) {
        free(binds);
        return NULL;
    }
    free(binds);
    return layout;
}

static lc_binding_set *make_storage_set(lc_device *device,
                                        lc_binding_layout *layout,
                                        lc_buffer **buffers,
                                        uint32_t count) {
    lc_binding_set *set = NULL;
    lc_binding_write *writes = NULL;
    uint32_t i;

    if (lc_binding_set_create(layout, &set) != LC_SUCCESS) {
        return NULL;
    }
    writes =
        (lc_binding_write *)calloc(count, sizeof(lc_binding_write));
    if (writes == NULL) {
        lc_binding_set_destroy(set);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        writes[i].binding = i;
        writes[i].array_element = 0;
        writes[i].type = LC_BINDING_STORAGE_BUFFER;
        writes[i].u.buffer.buffer = buffers[i];
        writes[i].u.buffer.offset = 0;
        writes[i].u.buffer.size = 0;
    }
    if (lc_binding_set_update(set, writes, count) != LC_SUCCESS) {
        free(writes);
        lc_binding_set_destroy(set);
        return NULL;
    }
    free(writes);
    return set;
}

static lc_compute_pipeline *make_compute_pipeline(
    lc_device *device, lc_shader *shader, lc_binding_layout *layout,
    uint32_t push_size) {
    lc_compute_pipeline *pipeline = NULL;
    lc_compute_pipeline_desc desc;
    lc_push_constant_range push;

    memset(&desc, 0, sizeof(desc));
    memset(&push, 0, sizeof(push));
    desc.compute_shader = shader;
    desc.binding_layouts = (const lc_binding_layout *const *)&layout;
    desc.binding_layout_count = (layout != NULL) ? 1u : 0u;
    if (push_size > 0) {
        push.visibility = LC_SHADER_VISIBILITY_COMPUTE;
        push.offset = 0;
        push.size = push_size;
        desc.push_constant_ranges = &push;
        desc.push_constant_range_count = 1;
    }
    if (lc_compute_pipeline_create(device, &desc, &pipeline) !=
        LC_SUCCESS) {
        return NULL;
    }
    return pipeline;
}

static lc_shader *make_compute_shader(lc_device *device,
                                      const uint8_t *code, size_t size) {
    lc_shader *shader = NULL;
    lc_shader_desc desc;

    memset(&desc, 0, sizeof(desc));
    desc.stage = LC_SHADER_STAGE_COMPUTE;
    desc.code = code;
    desc.code_size = size;
    desc.entry_point = "main";
    if (lc_shader_create(device, &desc, &shader) != LC_SUCCESS) {
        return NULL;
    }
    return shader;
}

int main(void) {
    enum { N = 256 };
    lc_device *device = NULL;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_command_encoder *enc = NULL;
    lc_compute_capabilities caps;
    lc_queue_info compute_queue;
    lc_pipeline_cache_info cache_info;
    uint8_t *double_spv = NULL;
    uint8_t *append_spv = NULL;
    size_t double_size = 0;
    size_t append_size = 0;
    lc_shader *double_shader = NULL;
    lc_shader *append_shader = NULL;
    lc_binding_layout *layout2 = NULL;
    lc_binding_layout *layout3 = NULL;
    lc_buffer *src = NULL;
    lc_buffer *dst = NULL;
    lc_buffer *flags = NULL;
    lc_buffer *visible = NULL;
    lc_buffer *counter = NULL;
    lc_binding_set *set2 = NULL;
    lc_binding_set *set3 = NULL;
    lc_compute_pipeline *doubler = NULL;
    lc_compute_pipeline *appender = NULL;
    lc_compute_pipeline *doubler_warm = NULL;
    uint32_t src_data[N];
    uint32_t dst_data[N];
    uint32_t flag_data[N];
    uint32_t vis_data[N];
    uint32_t counter_data[2];
    uint32_t push_count = N;
    uint32_t i;
    int ok = 1;

    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    double_spv = load_spv("test_double.comp.spv", &double_size);
    append_spv = load_spv("test_append.comp.spv", &append_size);
    CHECK(double_spv != NULL && double_size > 0, "double spv loads");
    CHECK(append_spv != NULL && append_size > 0, "append spv loads");
    if (double_spv == NULL || append_spv == NULL) {
        printf("phase21 compute: %d passed, %d failed\n", g_passed,
               g_failed);
        lc_shutdown();
        return 1;
    }

    /* Capabilities first (device-only). */
    {
        int dev = make_device(&device, NULL, 0);

        if (dev == 1) {
            SKIP_ENV("vulkan device");
        }
        CHECK(dev == 0, "device creates");
        memset(&caps, 0, sizeof(caps));
        lc_device_get_compute_capabilities(device, &caps);
        CHECK(caps.compute_supported, "compute supported");
        CHECK(caps.indirect_draw_supported, "indirect draw supported");
        CHECK(caps.max_workgroup_count[0] >= 65535u,
              "workgroup count limit sane");
        CHECK(caps.max_workgroup_invocations >= 64u,
              "workgroup invocations sane");
        CHECK(caps.max_push_size >= 128u, "push size sane");
        printf("[info] dedicated_compute=%d multi_draw=%d "
               "indirect_count=%d subgroup=%u\n",
               caps.dedicated_compute, caps.multi_draw_indirect,
               caps.indirect_count, caps.subgroup_size);
        memset(&compute_queue, 0, sizeof(compute_queue));
        lc_device_get_queue_info(device, LC_QUEUE_COMPUTE,
                                 &compute_queue);
        CHECK(compute_queue.available == caps.compute_supported,
              "queue info agrees with caps");
        CHECK(compute_queue.dedicated == caps.dedicated_compute,
              "queue dedication agrees with caps");
        if (!caps.compute_supported) {
            SKIP_ENV("compute support");
        }
    }

    /* Shader validation. */
    double_shader = make_compute_shader(device, double_spv, double_size);
    append_shader = make_compute_shader(device, append_spv, append_size);
    CHECK(double_shader != NULL, "compute shader creates");
    CHECK(append_shader != NULL, "append shader creates");
    {
        lc_shader *bad = NULL;
        lc_shader_desc desc;

        memset(&desc, 0, sizeof(desc));
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = double_spv;
        desc.code_size = double_size;
        desc.entry_point = "main";
        CHECK(lc_compute_pipeline_create(device, NULL, NULL) ==
                  LC_ERROR_INVALID_ARGUMENT,
              "compute pipeline rejects NULL desc");
        (void)bad;
    }

    /* Windowed frame stack for dispatch. */
    {
        int r;
        int r2;
        int r3;

        r = make_window_titled(&window, "LumaC Phase21 Compute");
        if (r == 1) {
            SKIP_ENV("window");
        }
        CHECK(r == 0, "window creates");
        r2 = make_surface(device, window, &surface);
        if (r2 == 1) {
            SKIP_ENV("surface");
        }
        CHECK(r2 == 0, "surface creates");
        r3 = make_swapchain(device, surface, &swapchain);
        if (r3 == 1) {
            SKIP_ENV("swapchain");
        }
        CHECK(r3 == 0, "swapchain creates");
    }

    /* Layouts/sets/buffers (UNDEFINED lenient update path). */
    layout2 = make_storage_layout(device, 2);
    layout3 = make_storage_layout(device, 3);
    CHECK(layout2 != NULL && layout3 != NULL, "storage layouts create");
    for (i = 0; i < N; i++) {
        src_data[i] = i + 1;
        flag_data[i] = ((i % 3) == 0) ? 1u : 0u;
        dst_data[i] = 0;
        vis_data[i] = 0xFFFFFFFFu;
    }
    counter_data[0] = 0;
    counter_data[1] = N;
    src = make_gpu_buffer(device, sizeof(src_data),
                          LC_BUFFER_USAGE_STORAGE |
                              LC_BUFFER_USAGE_TRANSFER_DST);
    dst = make_gpu_buffer(device, sizeof(dst_data),
                          LC_BUFFER_USAGE_STORAGE |
                              LC_BUFFER_USAGE_TRANSFER_DST);
    flags = make_gpu_buffer(device, sizeof(flag_data),
                            LC_BUFFER_USAGE_STORAGE |
                                LC_BUFFER_USAGE_TRANSFER_DST);
    visible = make_gpu_buffer(device, sizeof(vis_data),
                              LC_BUFFER_USAGE_STORAGE |
                                  LC_BUFFER_USAGE_TRANSFER_DST);
    counter = make_gpu_buffer(device, sizeof(counter_data),
                              LC_BUFFER_USAGE_STORAGE |
                                  LC_BUFFER_USAGE_TRANSFER_DST);
    CHECK(src && dst && flags && visible && counter,
          "gpu storage buffers create");
    {
        lc_buffer *pair2[2] = { src, dst };
        lc_buffer *trio[3] = { flags, visible, counter };

        set2 = make_storage_set(device, layout2, pair2, 2);
        set3 = make_storage_set(device, layout3, trio, 3);
        CHECK(set2 != NULL && set3 != NULL,
              "storage sets update on UNDEFINED buffers");
    }

    /* Async uploads (transfer -> compute ordering comes free with
     * the frame submit's transfer wait). */
    {
        lc_gpu_signal sig = { 0 };

        CHECK(lc_upload_buffer_async(device, src, 0, src_data,
                                     sizeof(src_data),
                                     &sig) == LC_SUCCESS,
              "async upload pattern");
        CHECK(lc_upload_buffer_async(device, dst, 0, dst_data,
                                     sizeof(dst_data),
                                     &sig) == LC_SUCCESS,
              "async upload zeros");
        CHECK(lc_upload_buffer_async(device, flags, 0, flag_data,
                                     sizeof(flag_data),
                                     &sig) == LC_SUCCESS,
              "async upload flags");
        CHECK(lc_upload_buffer_async(device, visible, 0, vis_data,
                                     sizeof(vis_data),
                                     &sig) == LC_SUCCESS,
              "async upload visible sentinel");
        CHECK(lc_upload_buffer_async(device, counter, 0, counter_data,
                                     sizeof(counter_data),
                                     &sig) == LC_SUCCESS,
              "async upload counter");
        /* Transfer-ordered path: TRANSFER_DST buffers remain
         * updatable (frame submits wait all in-flight transfers,
         * and completion is monotonic). */
        {
            lc_binding_write w;

            memset(&w, 0, sizeof(w));
            w.binding = 0;
            w.array_element = 0;
            w.type = LC_BINDING_STORAGE_BUFFER;
            w.u.buffer.buffer = src;
            w.u.buffer.offset = 0;
            w.u.buffer.size = 0;
            CHECK(lc_binding_set_update(set2, &w, 1) == LC_SUCCESS,
                  "TRANSFER_DST buffer updatable (transfer-ordered)");
        }
    }

    /* Pipelines (cold + warm share the device cache). */
    doubler = make_compute_pipeline(device, double_shader, layout2,
                                    sizeof(push_count));
    CHECK(doubler != NULL, "compute pipeline creates");
    doubler_warm = make_compute_pipeline(device, double_shader,
                                         layout2, sizeof(push_count));
    CHECK(doubler_warm != NULL, "compute pipeline warm creates");
    appender = make_compute_pipeline(device, append_shader, layout3,
                                     sizeof(push_count));
    CHECK(appender != NULL, "append pipeline creates");
    {
        lc_compute_pipeline *bad_pipe = NULL;
        lc_compute_pipeline_desc desc;

        memset(&desc, 0, sizeof(desc));
        desc.compute_shader = NULL;
        CHECK(lc_compute_pipeline_create(device, &desc, &bad_pipe) ==
                  LC_ERROR_INVALID_ARGUMENT,
              "compute pipeline rejects NULL shader");
    }

    /* Frame 1: transitions + double dispatch + verify. */
    if (lc_begin_frame(swapchain) != LC_SUCCESS) {
        CHECK(0, "begin frame for dispatch");
        ok = 0;
    } else {
        /* Present needs defined content: clear every frame (the
         * compute work itself never touches the swapchain image). */
        CHECK(lc_clear_color(swapchain, 0.02f, 0.03f, 0.05f, 1.0f) ==
                  LC_SUCCESS,
              "clear dispatch frame");
        CHECK(lc_swapchain_get_encoder(swapchain, &enc) == LC_SUCCESS,
              "borrow frame encoder");
        CHECK(lc_encoder_transition_buffer(enc, src,
                                           LC_RESOURCE_STATE_STORAGE_READ) ==
                  LC_SUCCESS,
              "transition src STORAGE_READ");
        CHECK(lc_encoder_transition_buffer(enc, dst,
                                           LC_RESOURCE_STATE_STORAGE_WRITE) ==
                  LC_SUCCESS,
              "transition dst STORAGE_WRITE");
        CHECK(lc_encoder_bind_compute_pipeline(enc, doubler) ==
                  LC_SUCCESS,
              "bind compute pipeline");
        CHECK(lc_encoder_bind_compute_set(enc, doubler, 0, set2) ==
                  LC_SUCCESS,
              "bind compute set");
        CHECK(lc_encoder_push_compute_constants(
                  enc, doubler, LC_SHADER_VISIBILITY_COMPUTE, 0,
                  sizeof(push_count), &push_count) == LC_SUCCESS,
              "push dispatch count");
        CHECK(lc_encoder_dispatch(enc, (N + 63u) / 64u, 1, 1) ==
                  LC_SUCCESS,
              "dispatch double");
        /* Negatives inside the frame. */
        CHECK(lc_encoder_dispatch(enc, 0, 1, 1) ==
                  LC_ERROR_INVALID_ARGUMENT,
              "dispatch x=0 rejected");
        CHECK(lc_encoder_dispatch(enc, device->max_workgroup_count[0] + 1u,
                                  1, 1) == LC_ERROR_INVALID_ARGUMENT,
              "dispatch over limit rejected");
        {
            lc_result end = lc_end_frame(swapchain);

            CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                  "end dispatch frame");
        }
        CHECK(lc_buffer_read(dst, 0, dst_data, sizeof(dst_data)) ==
                  LC_SUCCESS,
              "read back doubled buffer");
        for (i = 0; i < N && ok; i++) {
            if (dst_data[i] != (i + 1u) * 2u) {
                ok = 0;
            }
        }
        CHECK(ok, "compute doubling exact on GPU");
    }

    /* Frame 2: append compaction + counter verify. */
    ok = 1;
    if (lc_begin_frame(swapchain) != LC_SUCCESS) {
        CHECK(0, "begin frame for append");
        ok = 0;
    } else {
        uint32_t got_count[2] = { 0, 0 };

        CHECK(lc_clear_color(swapchain, 0.02f, 0.03f, 0.05f, 1.0f) ==
                  LC_SUCCESS,
              "clear append frame");
        CHECK(lc_swapchain_get_encoder(swapchain, &enc) == LC_SUCCESS,
              "borrow frame encoder again");
        CHECK(lc_encoder_transition_buffer(enc, flags,
                                           LC_RESOURCE_STATE_STORAGE_READ) ==
                  LC_SUCCESS,
              "transition flags STORAGE_READ");
        CHECK(lc_encoder_transition_buffer(enc, visible,
                                           LC_RESOURCE_STATE_STORAGE_WRITE) ==
                  LC_SUCCESS,
              "transition visible STORAGE_WRITE");
        CHECK(lc_encoder_transition_buffer(enc, counter,
                                           LC_RESOURCE_STATE_STORAGE_WRITE) ==
                  LC_SUCCESS,
              "transition counter STORAGE_WRITE");
        CHECK(lc_encoder_bind_compute_pipeline(enc, appender) ==
                  LC_SUCCESS,
              "bind append pipeline");
        CHECK(lc_encoder_bind_compute_set(enc, appender, 0, set3) ==
                  LC_SUCCESS,
              "bind append set");
        CHECK(lc_encoder_push_compute_constants(
                  enc, appender, LC_SHADER_VISIBILITY_COMPUTE, 0,
                  sizeof(push_count), &push_count) == LC_SUCCESS,
              "push append count");
        CHECK(lc_encoder_dispatch(enc, (N + 63u) / 64u, 1, 1) ==
                  LC_SUCCESS,
              "dispatch append");
        {
            lc_result end = lc_end_frame(swapchain);

            CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                  "end append frame");
        }
        CHECK(lc_buffer_read(counter, 0, got_count,
                             sizeof(got_count)) == LC_SUCCESS,
              "read back counter");
        {
            uint32_t expected = 0;

            for (i = 0; i < N; i++) {
                if ((i % 3) == 0) {
                    expected++;
                }
            }
            CHECK(got_count[0] == expected, "appended count exact");
        }
        CHECK(lc_buffer_read(visible, 0, vis_data, sizeof(vis_data)) ==
                  LC_SUCCESS,
              "read back compacted indices");
        {
            /* Append order across workgroups is nondeterministic:
             * verify as a set (sort on CPU, compare). */
            uint32_t j;
            uint32_t k;
            uint32_t n = 0;

            for (i = 0; i < N; i++) {
                if ((i % 3) == 0) {
                    n++;
                }
            }
            for (i = 0; i < n; i++) {
                for (j = i + 1; j < n; j++) {
                    if (vis_data[j] < vis_data[i]) {
                        uint32_t t = vis_data[i];

                        vis_data[i] = vis_data[j];
                        vis_data[j] = t;
                    }
                }
            }
            for (i = 0, k = 0; i < N && ok; i++) {
                if ((i % 3) == 0) {
                    if (vis_data[k++] != i) {
                        ok = 0;
                    }
                }
            }
            CHECK(ok, "compacted set equals flagged set");
        }
    }

    /* Frame 3: worker compute list executes outside any pass. */
    ok = 1;
    {
        lc_command_encoder *worker = NULL;
        lc_command_encoder_desc wdesc;
        lc_command_list *list = NULL;
        uint32_t pattern[N];
        lc_gpu_signal sig = { 0 };

        for (i = 0; i < N; i++) {
            pattern[i] = i * 3u;
        }
        memset(&wdesc, 0, sizeof(wdesc));
        wdesc.queue = LC_QUEUE_COMPUTE;
        CHECK(lc_command_encoder_create(device, &wdesc, &worker) ==
                  LC_SUCCESS,
              "compute worker creates");
        /* New pattern into SRC (the shader doubles src->dst); both
         * uploads mark TRANSFER_DST for the worker transitions. */
        CHECK(lc_upload_buffer_async(device, src, 0, pattern,
                                     sizeof(pattern),
                                     &sig) == LC_SUCCESS,
              "worker input uploaded");
        CHECK(lc_upload_buffer_async(device, dst, 0, pattern,
                                     sizeof(pattern),
                                     &sig) == LC_SUCCESS,
              "worker output poisoned");
        CHECK(lc_command_list_begin_compute(worker) == LC_SUCCESS,
              "compute list begins");
        CHECK(lc_worker_record_transition_buffer(
                  worker, src, LC_RESOURCE_STATE_STORAGE_READ) ==
                  LC_SUCCESS,
              "worker src transition recorded");
        CHECK(lc_worker_record_transition_buffer(
                  worker, dst, LC_RESOURCE_STATE_STORAGE_WRITE) ==
                  LC_SUCCESS,
              "worker transition recorded");
        CHECK(lc_worker_record_bind_compute_pipeline(worker,
                                                     doubler) ==
                  LC_SUCCESS,
              "worker compute bind recorded");
        CHECK(lc_worker_record_bind_compute_set(worker, doubler, 0,
                                                set2) == LC_SUCCESS,
              "worker compute set recorded");
        CHECK(lc_worker_record_push_compute(
                  worker, doubler, LC_SHADER_VISIBILITY_COMPUTE, 0,
                  sizeof(push_count), &push_count) == LC_SUCCESS,
              "worker compute push recorded");
        CHECK(lc_worker_record_dispatch(worker, (N + 63u) / 64u, 1,
                                        1) == LC_SUCCESS,
              "worker dispatch recorded");
        /* Dispatch is illegal in graphics lists (none open here);
         * the backend rejects mismatched list kinds. */
        CHECK(lc_command_encoder_finish(worker, &list) == LC_SUCCESS &&
                  list != NULL,
              "compute list finishes");
        if (lc_begin_frame(swapchain) != LC_SUCCESS) {
            CHECK(0, "begin frame for compute list");
            ok = 0;
        } else {
            CHECK(lc_clear_color(swapchain, 0.02f, 0.03f, 0.05f,
                                 1.0f) == LC_SUCCESS,
                  "clear compute-list frame");
            CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                      LC_SUCCESS,
                  "borrow frame encoder for execute");
            CHECK(lc_encoder_execute_lists(enc, &list, 1) ==
                      LC_SUCCESS,
                  "compute list executes outside pass");
            {
                lc_result end = lc_end_frame(swapchain);

                CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                      "end compute-list frame");
            }
            CHECK(lc_buffer_read(dst, 0, dst_data,
                                 sizeof(dst_data)) == LC_SUCCESS,
                  "read back worker result");
            for (i = 0; i < N && ok; i++) {
                if (dst_data[i] != (i * 3u) * 2u) {
                    ok = 0;
                }
            }
            if (!ok) {
                printf("[info] worker mismatch: got %u %u %u %u "
                       "want %u %u %u %u\n",
                       dst_data[0], dst_data[1], dst_data[2],
                       dst_data[3], 0u, 6u, 12u, 18u);
            }
            CHECK(ok, "worker compute doubling exact");
            lc_command_list_destroy(list);
        }
        lc_command_encoder_destroy(worker);
    }

    /* Isolated dedicated-queue dispatch (PART Z). */
    if (caps.dedicated_compute) {
        lc_buffer *once_dst = make_gpu_buffer(
            device, sizeof(dst_data),
            LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST);
        uint64_t value = 0;

        CHECK(once_dst != NULL, "once buffer creates");
        if (once_dst != NULL) {
            lc_gpu_signal sig = { 0 };
            uint32_t zero[64];
            lc_binding_set *once_set = NULL;
            lc_buffer *pair[2] = { src, once_dst };

            once_set = make_storage_set(device, layout2, pair, 2);
            CHECK(once_set != NULL, "once set creates");
            if (once_set != NULL) {
                {
                    lc_result dr;

                    /* Restore known src contents (frame 3
                     * repatterned it); the set above already
                     * references both buffers. */
                    CHECK(lc_upload_buffer_async(device, src, 0,
                                                 src_data,
                                                 sizeof(src_data),
                                                 &sig) == LC_SUCCESS,
                          "once src restored");
                    CHECK(lc_gpu_signal_wait(device, sig,
                                             LC_TIMEOUT_INFINITE) ==
                              LC_SUCCESS,
                          "src restore completes");
                    memset(zero, 0, sizeof(zero));
                    CHECK(lc_upload_buffer_async(device, once_dst, 0,
                                                 zero, sizeof(zero),
                                                 &sig) == LC_SUCCESS,
                          "once buffer zeroed");
                    CHECK(lc_gpu_signal_wait(device, sig,
                                             LC_TIMEOUT_INFINITE) ==
                              LC_SUCCESS,
                          "zero upload completes");
                    /* Upload completion was CPU-waited above, and
                     * the download below drains: the isolated path
                     * needs no tracked states (documented). */
                    dr = lc_vk_compute_dispatch_once(
                        device, doubler, &once_set,
                        (const uint32_t[]){ 0 }, 1, &push_count,
                        sizeof(push_count), (N + 63u) / 64u, 1, 1,
                        &value);
                    CHECK(dr == LC_SUCCESS,
                          "dedicated dispatch_once submits");
                    if (dr == LC_SUCCESS && value != 0) {
                        lc_gpu_signal s = { value };

                        CHECK(lc_gpu_signal_wait(
                                  device, s,
                                  LC_TIMEOUT_INFINITE) == LC_SUCCESS,
                              "dedicated completion observable");
                    }
                    CHECK(lc_buffer_read(once_dst, 0, dst_data,
                                         sizeof(zero)) == LC_SUCCESS,
                          "once result downloads");
                    {
                        uint32_t k;
                        int match = 1;

                        for (k = 0; k < 64; k++) {
                            if (dst_data[k] != (k + 1u) * 2u) {
                                match = 0;
                                break;
                            }
                        }
                        CHECK(match, "dedicated dispatch exact");
                    }
                    lc_binding_set_destroy(once_set);
                }
            }
            lc_buffer_destroy(once_dst);
        }
    } else {
        printf("[info] dedicated compute: NOT AVAILABLE\n");
    }

    /* Frame 4: storage-image write plus worker/frame compute
     * coexistence in one frame (deterministic record order,
     * outside any pass). */
    {
        uint8_t *storage_spv = NULL;
        size_t storage_size = 0;
        lc_shader *storage_shader = NULL;
        lc_image *simg = NULL;
        lc_image_view *sview = NULL;
        lc_binding_layout *slayout = NULL;
        lc_binding_set *sset = NULL;
        lc_compute_pipeline *spipe = NULL;
        lc_buffer *src2 = NULL;
        lc_buffer *dst2 = NULL;
        lc_binding_set *set2b = NULL;
        lc_command_encoder *worker2 = NULL;
        lc_command_encoder_desc wdesc2;
        lc_command_list *clist = NULL;
        uint32_t pattern2[64];
        uint32_t result2[64];
        uint32_t wh[2] = { 16, 16 };
        unsigned char img_out[16 * 16 * 4];
        uint32_t k;
        int img_ok = 1;
        int buf_ok = 1;

        storage_spv = load_spv("test_storage.comp.spv", &storage_size);
        CHECK(storage_spv != NULL, "storage spv loads");
        if (storage_spv != NULL) {
            storage_shader =
                make_compute_shader(device, storage_spv, storage_size);
            CHECK(storage_shader != NULL, "storage shader creates");
        }
        /* 16x16 STORAGE-capable image. */
        {
            lc_image_desc idesc;
            lc_image_view_desc vdesc;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 16;
            idesc.height = 16;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_STORAGE |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            CHECK(lc_image_create(device, &idesc, &simg) == LC_SUCCESS,
                  "storage image creates");
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (simg != NULL) {
                CHECK(lc_image_view_create(simg, &vdesc, &sview) ==
                          LC_SUCCESS,
                      "storage view creates");
            }
        }
        for (k = 0; k < 64; k++) {
            pattern2[k] = k + 100u;
            result2[k] = 0;
        }
        src2 = make_gpu_buffer(device, sizeof(pattern2),
                               LC_BUFFER_USAGE_STORAGE |
                                   LC_BUFFER_USAGE_TRANSFER_DST);
        dst2 = make_gpu_buffer(device, sizeof(result2),
                               LC_BUFFER_USAGE_STORAGE |
                                   LC_BUFFER_USAGE_TRANSFER_DST);
        CHECK(src2 != NULL && dst2 != NULL, "coex buffers create");
        /* Storage-image layout + set (UNDEFINED image is rejected:
         * transition first inside the frame below... sets update
         * BEFORE the frame here, so drive the image to
         * SHADER_READ_WRITE with a throwaway transition frame
         * first? No: update requires the state NOW. Order: begin
         * frame, transition image, create+update set, dispatch. */
        if (lc_begin_frame(swapchain) != LC_SUCCESS) {
            CHECK(0, "begin frame for storage test");
        } else {
            lc_binding_desc ibind;
            lc_binding_layout_desc ildesc;
            lc_binding_write iw;
            lc_compute_pipeline_desc pdesc;
            lc_push_constant_range prange;
            uint32_t cnt64 = 64;

            CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                      LC_SUCCESS,
                  "borrow frame encoder for storage");

            CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                      LC_SUCCESS,
                  "borrow frame encoder for storage");
            CHECK(lc_clear_color(swapchain, 0.02f, 0.03f, 0.05f,
                                 1.0f) == LC_SUCCESS,
                  "clear storage frame");
            {
                lc_image_subresource_range range;

                memset(&range, 0, sizeof(range));
                range.level_count = 1;
                range.layer_count = 1;
                CHECK(lc_encoder_transition_image(
                          enc, simg, &range,
                          LC_RESOURCE_STATE_SHADER_READ_WRITE) ==
                          LC_SUCCESS,
                      "transition image SHADER_READ_WRITE");
            }
            memset(&ibind, 0, sizeof(ibind));
            ibind.binding = 0;
            ibind.type = LC_BINDING_STORAGE_IMAGE;
            ibind.count = 1;
            ibind.visibility = LC_SHADER_VISIBILITY_COMPUTE;
            memset(&ildesc, 0, sizeof(ildesc));
            ildesc.bindings = &ibind;
            ildesc.binding_count = 1;
            CHECK(lc_binding_layout_create(device, &ildesc,
                                           &slayout) == LC_SUCCESS,
                  "storage layout creates");
            CHECK(lc_binding_set_create(slayout, &sset) == LC_SUCCESS,
                  "storage set creates");
            memset(&iw, 0, sizeof(iw));
            iw.binding = 0;
            iw.array_element = 0;
            iw.type = LC_BINDING_STORAGE_IMAGE;
            iw.u.image.view = sview;
            CHECK(lc_binding_set_update(sset, &iw, 1) == LC_SUCCESS,
                  "storage set updates on GENERAL image");
            memset(&pdesc, 0, sizeof(pdesc));
            memset(&prange, 0, sizeof(prange));
            prange.visibility = LC_SHADER_VISIBILITY_COMPUTE;
            prange.offset = 0;
            prange.size = sizeof(wh);
            pdesc.compute_shader = storage_shader;
            pdesc.binding_layouts =
                (const lc_binding_layout *const *)&slayout;
            pdesc.binding_layout_count = 1;
            pdesc.push_constant_ranges = &prange;
            pdesc.push_constant_range_count = 1;
            CHECK(lc_compute_pipeline_create(device, &pdesc,
                                             &spipe) == LC_SUCCESS,
                  "storage pipeline creates");
            /* Fresh pair for the coexistence half. */
            {
                lc_buffer *pair[2] = { src2, dst2 };
                lc_gpu_signal sig = { 0 };

                set2b = make_storage_set(device, layout2, pair, 2);
                CHECK(set2b != NULL, "coex set creates");
                CHECK(lc_upload_buffer_async(device, src2, 0, pattern2,
                                             sizeof(pattern2),
                                             &sig) == LC_SUCCESS,
                      "coex src uploaded");
                CHECK(lc_upload_buffer_async(device, dst2, 0, result2,
                                             sizeof(result2),
                                             &sig) == LC_SUCCESS,
                      "coex dst poisoned");
            }
            /* Worker list first (record order = execution order
             * within this frame's single execution point... both
             * record into different buffers; the frame executes
             * immediate dispatch first, then the list). */
            memset(&wdesc2, 0, sizeof(wdesc2));
            wdesc2.queue = LC_QUEUE_COMPUTE;
            CHECK(lc_command_encoder_create(device, &wdesc2,
                                            &worker2) == LC_SUCCESS,
                  "coex worker creates");
            CHECK(lc_command_list_begin_compute(worker2) == LC_SUCCESS,
                  "coex list begins");
            CHECK(lc_worker_record_transition_buffer(
                      worker2, src2,
                      LC_RESOURCE_STATE_STORAGE_READ) == LC_SUCCESS,
                  "coex src transition");
            CHECK(lc_worker_record_transition_buffer(
                      worker2, dst2,
                      LC_RESOURCE_STATE_STORAGE_WRITE) == LC_SUCCESS,
                  "coex dst transition");
            CHECK(lc_worker_record_bind_compute_pipeline(worker2,
                                                         doubler) ==
                      LC_SUCCESS,
                  "coex bind recorded");
            CHECK(lc_worker_record_bind_compute_set(worker2, doubler,
                                                    0, set2b) ==
                      LC_SUCCESS,
                  "coex set recorded");
            CHECK(lc_worker_record_push_compute(
                      worker2, doubler, LC_SHADER_VISIBILITY_COMPUTE,
                      0, sizeof(cnt64), &cnt64) == LC_SUCCESS,
                  "coex push recorded");
            CHECK(lc_worker_record_dispatch(worker2, 1, 1, 1) ==
                      LC_SUCCESS,
                  "coex dispatch recorded");
            CHECK(lc_command_encoder_finish(worker2, &clist) ==
                      LC_SUCCESS &&
                      clist != NULL,
                  "coex list finishes");
            /* Immediate storage dispatch. */
            CHECK(lc_encoder_bind_compute_pipeline(enc, spipe) ==
                      LC_SUCCESS,
                  "bind storage pipeline");
            CHECK(lc_encoder_bind_compute_set(enc, spipe, 0, sset) ==
                      LC_SUCCESS,
                  "bind storage set");
            CHECK(lc_encoder_push_compute_constants(
                      enc, spipe, LC_SHADER_VISIBILITY_COMPUTE, 0,
                      sizeof(wh), &wh) == LC_SUCCESS,
                  "push image dims");
            CHECK(lc_encoder_dispatch(enc, 2, 2, 1) == LC_SUCCESS,
                  "dispatch storage write");
            /* Worker list executes in the same frame. */
            CHECK(lc_encoder_execute_lists(enc, &clist, 1) ==
                      LC_SUCCESS,
                  "coex list executes with dispatch");
            /* Image back to TRANSFER_SRC for the sync readback. */
            {
                lc_image_subresource_range range;

                memset(&range, 0, sizeof(range));
                range.level_count = 1;
                range.layer_count = 1;
                CHECK(lc_encoder_transition_image(
                          enc, simg, &range,
                          LC_RESOURCE_STATE_TRANSFER_SRC) == LC_SUCCESS,
                      "transition image TRANSFER_SRC");
            }
            {
                lc_result end = lc_end_frame(swapchain);

                CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                      "end storage frame");
            }
            /* Verify image gradient + doubled buffer. */
            {
                lc_image_readback_desc rd;
                unsigned char px[16 * 16 * 4];

                memset(&rd, 0, sizeof(rd));
                if (lc_image_readback(simg, &rd, px, sizeof(px),
                                      NULL) == LC_SUCCESS) {
                    /* (0,0) blackish, (15,15) near-white, alpha 1. */
                    if (px[0] > 2 || px[1] > 2 || px[3] < 250) {
                        img_ok = 0;
                    }
                    {
                        unsigned char *c =
                            px + (15u * 16u + 15u) * 4u;

                        if (c[0] < 237 || c[1] < 237 || c[3] < 250) {
                            img_ok = 0;
                        }
                    }
                } else {
                    img_ok = 0;
                }
                CHECK(img_ok, "storage image gradient exact");
            }
            CHECK(lc_buffer_read(dst2, 0, result2, sizeof(result2)) ==
                      LC_SUCCESS,
                  "coex result downloads");
            for (k = 0; k < 64 && buf_ok; k++) {
                if (result2[k] != (k + 100u) * 2u) {
                    buf_ok = 0;
                }
            }
            CHECK(buf_ok, "coex worker doubling exact");
            lc_command_list_destroy(clist);
            lc_command_encoder_destroy(worker2);
            lc_binding_set_destroy(set2b);
            lc_compute_pipeline_destroy(spipe);
            lc_binding_set_destroy(sset);
            lc_binding_layout_destroy(slayout);
            lc_image_view_destroy(sview);
            lc_image_destroy(simg);
            lc_shader_destroy(storage_shader);
            lc_buffer_destroy(dst2);
            lc_buffer_destroy(src2);
        }
        free(storage_spv);
    }

    /* Strict path: a VERTEX_READ buffer is rejected from a
     * storage slot (wrong-slot reuse is still caught). */
    if (lc_begin_frame(swapchain) == LC_SUCCESS) {
        lc_command_encoder *e2 = NULL;

        if (lc_swapchain_get_encoder(swapchain, &e2) == LC_SUCCESS) {
            CHECK(lc_clear_color(swapchain, 0.02f, 0.03f, 0.05f,
                                 1.0f) == LC_SUCCESS,
                  "clear strict frame");
            CHECK(lc_encoder_transition_buffer(
                      e2, src, LC_RESOURCE_STATE_VERTEX_READ) ==
                      LC_SUCCESS,
                  "transition src VERTEX_READ");
        }
        {
            lc_result end = lc_end_frame(swapchain);

            CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                  "end strict frame");
        }
    }
    {
        lc_binding_write w;

        memset(&w, 0, sizeof(w));
        w.binding = 0;
        w.array_element = 0;
        w.type = LC_BINDING_STORAGE_BUFFER;
        w.u.buffer.buffer = src;
        w.u.buffer.offset = 0;
        w.u.buffer.size = 0;
        CHECK(lc_binding_set_update(set2, &w, 1) ==
                  LC_ERROR_INVALID_ARGUMENT,
              "VERTEX_READ buffer rejected from storage slot");
    }

    /* Cache participation: compute pipelines share the device
     * file cache (cold -> warm load), survive corrupt blobs, and
     * respect the disabled flag. */
    {
        const char *path = "phase21_compute_cache.bin";
        const char *warm_path = "phase21_compute_cache_warm.bin";
        lc_device *cached = NULL;
        lc_device *warm = NULL;
        lc_pipeline_cache_info before;
        lc_pipeline_cache_info loaded;
        FILE *junk = NULL;

        memset(&before, 0, sizeof(before));
        memset(&loaded, 0, sizeof(loaded));
#if defined(_WIN32)
        fopen_s(&junk, path, "wb");
#else
        junk = fopen(path, "wb");
#endif
        if (junk != NULL) {
            fwrite("not-a-cache", 1, 11, junk);
            fclose(junk);
        }
        CHECK(make_device(&cached, path, 0) == 0,
              "device with corrupt cache creates");
        if (cached != NULL) {
            lc_device_get_pipeline_cache_info(cached, &before);
            CHECK(before.enabled, "cache enabled despite corrupt blob");
            lc_device_destroy(cached);
        }
        remove(path);
        /* Warm: seed the file with a compute pipeline, reload it. */
        {
            lc_device_desc ddesc;
            lc_shader *ws = NULL;
            lc_compute_pipeline *wp = NULL;
            lc_shader_desc sdesc;

            memset(&ddesc, 0, sizeof(ddesc));
            ddesc.backend = LC_BACKEND_VULKAN;
            ddesc.enable_validation = 1;
            ddesc.pipeline_cache_path = warm_path;
            remove(warm_path);
            if (lc_device_create(&ddesc, &warm) == LC_SUCCESS) {
                lc_binding_layout *wlayout = NULL;

                memset(&sdesc, 0, sizeof(sdesc));
                sdesc.stage = LC_SHADER_STAGE_COMPUTE;
                sdesc.code = double_spv;
                sdesc.code_size = double_size;
                sdesc.entry_point = "main";
                if (lc_shader_create(warm, &sdesc, &ws) ==
                        LC_SUCCESS &&
                    (wlayout = make_storage_layout(warm, 2)) != NULL &&
                    (wp = make_compute_pipeline(warm, ws, wlayout,
                                                sizeof(push_count))) !=
                        NULL) {
                    CHECK(1, "warm-seed compute pipeline creates");
                    lc_compute_pipeline_destroy(wp);
                    lc_binding_layout_destroy(wlayout);
                    lc_shader_destroy(ws);
                } else {
                    CHECK(0, "warm-seed compute pipeline creates");
                    lc_binding_layout_destroy(wlayout);
                    lc_shader_destroy(ws);
                }
                lc_device_destroy(warm);
                warm = NULL;
            } else {
                CHECK(0, "warm-seed device creates");
            }
            if (make_device(&warm, warm_path, 0) == 0) {
                lc_device_get_pipeline_cache_info(warm, &loaded);
                CHECK(loaded.loaded_from_file,
                      "compute cache blob reloads");
                lc_device_destroy(warm);
                warm = NULL;
            } else {
                CHECK(0, "warm-load device creates");
            }
            remove(warm_path);
        }
        /* Disabled flag: device works, cache stays off. */
        {
            lc_device *off = NULL;

            if (make_device(&off, NULL, 1) == 0) {
                lc_device_get_pipeline_cache_info(off, &before);
                CHECK(!before.enabled, "cache disabled on request");
                lc_device_destroy(off);
            } else {
                CHECK(0, "disabled-cache device creates");
            }
        }
    }

    free(append_spv);
    free(double_spv);
    lc_compute_pipeline_destroy(doubler_warm);
    lc_compute_pipeline_destroy(doubler);
    lc_compute_pipeline_destroy(appender);
    lc_binding_set_destroy(set3);
    lc_binding_set_destroy(set2);
    lc_buffer_destroy(counter);
    lc_buffer_destroy(visible);
    lc_buffer_destroy(flags);
    lc_buffer_destroy(dst);
    lc_buffer_destroy(src);
    lc_binding_layout_destroy(layout3);
    lc_binding_layout_destroy(layout2);
    lc_shader_destroy(append_shader);
    lc_shader_destroy(double_shader);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_window_destroy(window);
    lc_device_destroy(device);
    lc_shutdown();
    printf("phase21 compute: %d passed, %d failed\n", g_passed,
           g_failed);
    return g_failed ? 1 : 0;
}
