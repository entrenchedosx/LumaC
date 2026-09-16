/*
 * Vulkan indirect-count test (Phase 23).
 *
 * Compute-style GPU-written draw counts consumed by graphics with
 * no CPU readback: four fullscreen-triangle commands painted from
 * a per-command push palette (painter order decides the pixel).
 * count = 3 shows the third color, count = 0 shows clear, an
 * oversized count clamps to the bound. Native
 * vkCmdDraw*IndirectCount runs where the capability reports it;
 * elsewhere the fixed-count fallback draws the bound with
 * zero-instance no-ops (same pixels either way). Misuse matrix +
 * one worker-list count draw included. Validation layers enabled
 * throughout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <lumac/lumac.h>

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

static int load_spv(const char *name, void **out, size_t *out_size) {
    char path[512];
    FILE *file = NULL;
    long length = 0;
    void *code = NULL;
    int written = snprintf(path, sizeof(path), "%s/%s",
                           LC_TRIANGLE_SPV_DIR, name);

    if (written < 0 || (size_t)written >= sizeof(path)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || (length % 4) != 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    code = malloc((size_t)length);
    if (code == NULL) {
        fclose(file);
        return 0;
    }
    if (fread(code, 1, (size_t)length, file) != (size_t)length) {
        free(code);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = code;
    *out_size = (size_t)length;
    return 1;
}

enum { W = 64, H = 64, COMMANDS = 4 };

/* Palette: red, green, blue, white. count=N shows color[N-1]. */
static const float g_palette[4][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f }
};

static int pixel_is(const uint8_t *px, uint8_t r, uint8_t g,
                    uint8_t b) {
    return px[0] == r && px[1] == g && px[2] == b && px[3] == 255;
}

/* One counted frame: transition, clear, push the palette, draw,
 * present. Returns 1 when every step succeeds (CHECKs fire
 * inside). */
static int run_count_case(lc_swapchain *swapchain,
                          lc_command_encoder **out_primary,
                          lc_render_target *target,
                          const lc_render_pass_desc *pass,
                          lc_pipeline *pipeline, lc_buffer *commands,
                          lc_buffer *count, const char *msg) {
    lc_command_encoder *primary = NULL;

    (void)target;
    if (lc_begin_frame(swapchain) != LC_SUCCESS ||
        lc_swapchain_get_encoder(swapchain, &primary) !=
            LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_transition_buffer(primary, commands,
                                     LC_RESOURCE_STATE_INDIRECT_READ) !=
            LC_SUCCESS ||
        lc_encoder_transition_buffer(primary, count,
                                     LC_RESOURCE_STATE_INDIRECT_READ) !=
            LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_begin_render_pass(primary, pass) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_bind_pipeline(primary, pipeline) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_push_constants(primary, pipeline,
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_VERTEX |
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_FRAGMENT,
                                  0, (uint32_t)sizeof(g_palette),
                                  g_palette) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_draw_indirect_count(primary, commands, 0, count,
                                       0, COMMANDS,
                                       16) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_end_render_pass(primary) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    {
        lc_result end_res = lc_end_frame(swapchain);

        if (end_res != LC_SUCCESS && end_res != LC_SUBOPTIMAL) {
            CHECK(0, msg);
            return 0;
        }
    }
    *out_primary = primary;
    return 1;
}

/* Indexed variant: binds the index buffer, then one counted
 * indexed draw. Returns 1 on full success. */
static int run_indexed_case(lc_swapchain *swapchain,
                            lc_command_encoder **out_primary,
                            const lc_render_pass_desc *pass,
                            lc_pipeline *pipeline,
                            lc_buffer *icommands, lc_buffer *ibo,
                            lc_buffer *count, const char *msg) {
    lc_command_encoder *primary = NULL;

    if (lc_begin_frame(swapchain) != LC_SUCCESS ||
        lc_swapchain_get_encoder(swapchain, &primary) !=
            LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_transition_buffer(primary, icommands,
                                     LC_RESOURCE_STATE_INDIRECT_READ) !=
            LC_SUCCESS ||
        lc_encoder_transition_buffer(primary, ibo,
                                     LC_RESOURCE_STATE_INDEX_READ) !=
            LC_SUCCESS ||
        lc_encoder_transition_buffer(primary, count,
                                     LC_RESOURCE_STATE_INDIRECT_READ) !=
            LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_begin_render_pass(primary, pass) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_bind_pipeline(primary, pipeline) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_push_constants(primary, pipeline,
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_VERTEX |
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_FRAGMENT,
                                  0, (uint32_t)sizeof(g_palette),
                                  g_palette) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_bind_index_buffer(primary, ibo, 0,
                                     LC_INDEX_UINT32) !=
        LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_draw_indexed_indirect_count(
            primary, icommands, 0, count, 0, COMMANDS, 20) !=
        LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    if (lc_encoder_end_render_pass(primary) != LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    {
        lc_result end_res = lc_end_frame(swapchain);

        if (end_res != LC_SUCCESS && end_res != LC_SUBOPTIMAL) {
            CHECK(0, msg);
            return 0;
        }
    }
    *out_primary = primary;
    return 1;
}

int main(void) {
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
    lc_push_constant_range push_range;
    lc_pipeline *pipeline = NULL;
    lc_render_color_attachment color;
    lc_render_pass_desc pass;
    lc_command_encoder *primary = NULL;
    lc_buffer_desc bdesc;
    lc_buffer *commands = NULL;
    lc_buffer *count = NULL;
    lc_buffer *plain = NULL;
    lc_buffer *icommands = NULL;
    lc_buffer *ibo = NULL;
    lc_image_readback_desc rb;
    uint8_t pixels[W * H * 4];
    lc_compute_capabilities caps;
    void *vs_code = NULL, *fs_code = NULL;
    size_t vs_size = 0, fs_size = 0;
    uint32_t i;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC indirect-count (Phase 23) test...\n");
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &device) != LC_SUCCESS) {
        printf("SKIP: no Vulkan device\n");
        lc_shutdown();
        return 0;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Indirect Count";
    wdesc.width = 320;
    wdesc.height = 240;
    if (lc_window_create(&wdesc, &window) != LC_SUCCESS ||
        lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        printf("SKIP: windowed Vulkan unavailable\n");
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 0;
    }
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 320;
    sdesc.height = 240;
    sdesc.vsync = 1;
    if (lc_swapchain_create(device, surface, &sdesc, &swapchain) !=
        LC_SUCCESS) {
        printf("SKIP: windowed Vulkan unavailable\n");
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 0;
    }
    memset(&caps, 0, sizeof(caps));
    lc_device_get_compute_capabilities(device, &caps);
    printf("[info] indirect_count=%d multi_draw=%d\n",
           caps.indirect_count, caps.multi_draw_indirect);
    CHECK(caps.indirect_draw_supported, "indirect draws supported");

    /* Offscreen RGBA8 target (pixel oracle, no presentation). */
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = W;
    idesc.height = H;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_SAMPLED;
    idesc.samples = LC_SAMPLE_COUNT_1;
    CHECK(lc_image_create(device, &idesc, &image) == LC_SUCCESS,
          "offscreen image creates");
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    CHECK(lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS,
          "offscreen view creates");
    memset(&rtdesc, 0, sizeof(rtdesc));
    rtatt.view = view;
    rtdesc.color_attachments = &rtatt;
    rtdesc.color_attachment_count = 1;
    rtdesc.width = W;
    rtdesc.height = H;
    CHECK(lc_render_target_create(device, &rtdesc, &target) ==
              LC_SUCCESS,
          "offscreen target creates");
    if (!load_spv("count_test.vert.spv", &vs_code, &vs_size) ||
        !load_spv("count_test.frag.spv", &fs_code, &fs_size)) {
        printf("count test SPIR-V missing: FAIL\n");
        return 1;
    }
    {
        lc_shader_desc sdesc2;

        memset(&sdesc2, 0, sizeof(sdesc2));
        sdesc2.stage = LC_SHADER_STAGE_VERTEX;
        sdesc2.code = vs_code;
        sdesc2.code_size = vs_size;
        CHECK(lc_shader_create(device, &sdesc2, &vs) == LC_SUCCESS,
              "count vertex shader creates");
        sdesc2.stage = LC_SHADER_STAGE_FRAGMENT;
        sdesc2.code = fs_code;
        sdesc2.code_size = fs_size;
        CHECK(lc_shader_create(device, &sdesc2, &fs) == LC_SUCCESS,
              "count fragment shader creates");
    }
    free(vs_code);
    free(fs_code);
    memset(&pldesc, 0, sizeof(pldesc));
    pldesc.vertex_shader = vs;
    pldesc.fragment_shader = fs;
    pldesc.cull_mode = LC_CULL_NONE;
    push_range.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                            (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    push_range.offset = 0;
    push_range.size = (uint32_t)sizeof(g_palette);
    pldesc.push_constant_ranges = &push_range;
    pldesc.push_constant_range_count = 1;
    pldesc.render_target.color_attachment_count = 1;
    pldesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    pldesc.render_target.samples = LC_SAMPLE_COUNT_1;
    CHECK(lc_graphics_pipeline_create(device, &pldesc, &pipeline) ==
              LC_SUCCESS,
          "count pipeline creates");
    lc_shader_destroy(vs);
    lc_shader_destroy(fs);

    /* Command + count + plain buffers. */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)COMMANDS * 16u;
    bdesc.usage = LC_BUFFER_USAGE_INDIRECT | LC_BUFFER_USAGE_STORAGE |
                  LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &commands) == LC_SUCCESS,
          "command buffer creates");
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = 4;
    bdesc.usage = LC_BUFFER_USAGE_INDIRECT | LC_BUFFER_USAGE_STORAGE |
                  LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &count) == LC_SUCCESS,
          "count buffer creates");
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = 64;
    bdesc.usage = LC_BUFFER_USAGE_STORAGE;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &plain) == LC_SUCCESS,
          "plain buffer creates");
    /* Indexed fixtures: identity indices + indexed commands. */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)COMMANDS * 20u;
    bdesc.usage = LC_BUFFER_USAGE_INDIRECT | LC_BUFFER_USAGE_STORAGE |
                  LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &icommands) == LC_SUCCESS,
          "indexed command buffer creates");
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)COMMANDS * 3u * sizeof(uint32_t);
    bdesc.usage = LC_BUFFER_USAGE_INDEX | LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &ibo) == LC_SUCCESS,
          "index buffer creates");
    {
        lc_indirect_draw_indexed_command icmds[COMMANDS];
        uint32_t idx[COMMANDS * 3];
        uint32_t k;

        for (k = 0; k < COMMANDS * 3u; k++) {
            idx[k] = k;
        }
        for (i = 0; i < COMMANDS; i++) {
            icmds[i].index_count = 3;
            icmds[i].instance_count = 1;
            icmds[i].first_index = i * 3u;
            icmds[i].vertex_offset = 0;
            icmds[i].first_instance = 0;
        }
        CHECK(lc_buffer_write(icommands, 0, icmds, sizeof(icmds)) ==
                  LC_SUCCESS,
              "indexed commands upload");
        CHECK(lc_buffer_write(ibo, 0, idx, sizeof(idx)) ==
                  LC_SUCCESS,
              "indices upload");
    }
    {
        lc_indirect_draw_command cmds[COMMANDS];

        for (i = 0; i < COMMANDS; i++) {
            cmds[i].vertex_count = 3;
            cmds[i].instance_count = 1;
            cmds[i].first_vertex = i * 3u;
            cmds[i].first_instance = 0;
        }
        CHECK(lc_buffer_write(commands, 0, cmds, sizeof(cmds)) ==
                  LC_SUCCESS,
              "commands upload");
    }
    memset(&color, 0, sizeof(color));
    color.view = view;
    color.load_op = LC_LOAD_OP_CLEAR;
    color.store_op = LC_STORE_OP_STORE;
    color.clear_color[0] = 0.0f;
    color.clear_color[1] = 0.0f;
    color.clear_color[2] = 0.0f;
    color.clear_color[3] = 1.0f;
    memset(&pass, 0, sizeof(pass));
    pass.color_attachments = &color;
    pass.color_attachment_count = 1;
    pass.width = W;
    pass.height = H;

    /* Misuse matrix (no frame needed for pure validation). */
    CHECK(lc_encoder_draw_indirect_count(NULL, commands, 0, count, 0,
                                         COMMANDS,
                                         16) ==
              LC_ERROR_INVALID_ARGUMENT,
          "NULL encoder rejected");
    CHECK(lc_encoder_draw_indexed_indirect_count(
              NULL, commands, 0, count, 0, COMMANDS, 20) ==
              LC_ERROR_INVALID_ARGUMENT,
          "NULL encoder rejected (indexed)");

    /* count = 3 draws red, green, blue: center must be blue. */
    {
        uint32_t three = 3;

        CHECK(lc_buffer_write(count, 0, &three, sizeof(three)) ==
                  LC_SUCCESS,
              "count three uploads");
        CHECK(run_count_case(swapchain, &primary, target, &pass,
                             pipeline, commands, count,
                             "count three shows third color") == 1,
              "count three shows third color");
        memset(pixels, 0, sizeof(pixels));
        memset(&rb, 0, sizeof(rb));
        CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels),
                                NULL) == LC_SUCCESS,
              "count three reads back");
        CHECK(pixel_is(&pixels[((H / 2) * W + W / 2) * 4], 0, 0,
                       255),
              "count three pixel is blue");
    }
    /* count = 0 renders nothing: clear survives. */
    {
        uint32_t zero = 0;

        CHECK(lc_buffer_write(count, 0, &zero, sizeof(zero)) ==
                  LC_SUCCESS,
              "count zero uploads");
        CHECK(run_count_case(swapchain, &primary, target, &pass,
                             pipeline, commands, count,
                             "count zero draws nothing") == 1,
              "count zero draws nothing");
        memset(pixels, 0, sizeof(pixels));
        memset(&rb, 0, sizeof(rb));
        CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels),
                                NULL) == LC_SUCCESS,
              "count zero reads back");
        CHECK(pixel_is(&pixels[((H / 2) * W + W / 2) * 4], 0, 0, 0),
              "count zero pixel stays clear");
    }
    /* Oversized count clamps to the bound: white wins. */
    {
        uint32_t huge = 99;

        CHECK(lc_buffer_write(count, 0, &huge, sizeof(huge)) ==
                  LC_SUCCESS,
              "count oversized uploads");
        CHECK(run_count_case(swapchain, &primary, target, &pass,
                             pipeline, commands, count,
                             "count clamps to bound") == 1,
              "count clamps to bound");
        memset(pixels, 0, sizeof(pixels));
        memset(&rb, 0, sizeof(rb));
        CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels),
                                NULL) == LC_SUCCESS,
              "count max reads back");
        CHECK(pixel_is(&pixels[((H / 2) * W + W / 2) * 4], 255, 255,
                       255),
              "clamped count pixel is white");
    }
    /* Indexed count draws execute with identical pixels. */
    {
        uint32_t three = 3;

        CHECK(lc_buffer_write(count, 0, &three, sizeof(three)) ==
                  LC_SUCCESS,
              "indexed count uploads");
        CHECK(run_indexed_case(swapchain, &primary, &pass,
                               pipeline, icommands, ibo, count,
                               "indexed count draws") == 1,
              "indexed count draws");
        memset(pixels, 0, sizeof(pixels));
        memset(&rb, 0, sizeof(rb));
        CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels),
                                NULL) == LC_SUCCESS,
              "indexed output reads back");
        CHECK(pixel_is(&pixels[((H / 2) * W + W / 2) * 4], 0, 0,
                       255),
              "indexed count pixel is blue");
    }
    /* State misuse: park the count buffer outside INDIRECT_READ
     * on a live frame, then prove the draw rejects it. */
    CHECK(lc_begin_frame(swapchain) == LC_SUCCESS &&
              lc_swapchain_get_encoder(swapchain, &primary) ==
                  LC_SUCCESS,
          "misuse frame begins");
    CHECK(lc_encoder_transition_buffer(primary, count,
                                       LC_RESOURCE_STATE_STORAGE_READ) ==
              LC_SUCCESS,
          "count buffer parked in STORAGE_READ");
    CHECK(lc_encoder_begin_render_pass(primary, &pass) ==
              LC_SUCCESS,
          "misuse pass begins");
    CHECK(lc_encoder_bind_pipeline(primary, pipeline) == LC_SUCCESS,
          "misuse pipeline binds");
    CHECK(lc_encoder_push_constants(primary, pipeline,
                                    (uint32_t)
                                        LC_SHADER_VISIBILITY_VERTEX |
                                    (uint32_t)
                                        LC_SHADER_VISIBILITY_FRAGMENT,
                                    0, (uint32_t)sizeof(g_palette),
                                    g_palette) == LC_SUCCESS,
          "misuse palette pushes");
    CHECK(lc_encoder_draw_indirect_count(primary, commands, 0, count,
                                         0, COMMANDS,
                                         16) ==
              LC_ERROR_INVALID_ARGUMENT,
          "non-INDIRECT count buffer rejected");
    CHECK(lc_encoder_draw_indirect_count(primary, commands, 0, plain,
                                         0, COMMANDS,
                                         16) ==
              LC_ERROR_INVALID_ARGUMENT,
          "non-INDIRECT usage rejected");
    CHECK(lc_encoder_draw_indirect_count(primary, commands, 0, count,
                                         2, COMMANDS,
                                         16) ==
              LC_ERROR_INVALID_ARGUMENT,
          "misaligned count offset rejected");
    CHECK(lc_encoder_draw_indirect_count(primary, commands, 0, count,
                                         0, 0,
                                         16) ==
              LC_ERROR_INVALID_ARGUMENT,
          "zero max count rejected");
    CHECK(lc_encoder_end_render_pass(primary) == LC_SUCCESS,
          "misuse pass ends");
    {
        lc_result end_res = lc_end_frame(swapchain);

        CHECK(end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL,
              "misuse frame ends");
    }
    /* Worker-list count draw executes clean (same backend emit,
     * secondary command buffer). */
    {
        lc_command_encoder_desc edesc;
        lc_command_encoder *worker = NULL;
        lc_command_list *list = NULL;
        uint32_t one = 1;

        CHECK(lc_buffer_write(count, 0, &one, sizeof(one)) ==
                  LC_SUCCESS,
              "worker count uploads");
        memset(&edesc, 0, sizeof(edesc));
        edesc.queue = LC_QUEUE_GRAPHICS;
        CHECK(lc_command_encoder_create(device, &edesc, &worker) ==
                  LC_SUCCESS,
              "worker encoder creates");
        CHECK(lc_begin_frame(swapchain) == LC_SUCCESS &&
                  lc_swapchain_get_encoder(swapchain, &primary) ==
                      LC_SUCCESS,
              "worker frame begins");
        CHECK(lc_encoder_transition_buffer(
                  primary, commands,
                  LC_RESOURCE_STATE_INDIRECT_READ) == LC_SUCCESS &&
                  lc_encoder_transition_buffer(
                      primary, count,
                      LC_RESOURCE_STATE_INDIRECT_READ) == LC_SUCCESS,
              "worker buffers transition");
        CHECK(lc_command_list_begin(worker, target, &pass) ==
                  LC_SUCCESS,
              "worker list begins");
        CHECK(lc_encoder_bind_pipeline(worker, pipeline) ==
                  LC_SUCCESS,
              "worker pipeline binds");
        CHECK(lc_encoder_push_constants(worker, pipeline,
                                        (uint32_t)
                                            LC_SHADER_VISIBILITY_VERTEX |
                                        (uint32_t)
                                            LC_SHADER_VISIBILITY_FRAGMENT,
                                        0,
                                        (uint32_t)sizeof(g_palette),
                                        g_palette) == LC_SUCCESS,
              "worker palette pushes");
        CHECK(lc_encoder_draw_indirect_count(worker, commands, 0,
                                             count, 0, 1,
                                             16) == LC_SUCCESS,
              "worker count draw records");
        CHECK(lc_encoder_draw_indexed_indirect_count(
                  worker, commands, 0, count, 0, 0, 20) ==
                  LC_ERROR_INVALID_ARGUMENT,
              "worker zero max rejected");
        CHECK(lc_command_encoder_finish(worker, &list) == LC_SUCCESS,
              "worker list finishes");
        CHECK(lc_encoder_begin_render_pass(primary, &pass) ==
                  LC_SUCCESS,
              "worker pass begins");
        CHECK(lc_encoder_execute_lists(primary, &list, 1) ==
                  LC_SUCCESS,
              "worker list executes");
        CHECK(lc_encoder_end_render_pass(primary) == LC_SUCCESS,
              "worker pass ends");
        {
            lc_result end_res = lc_end_frame(swapchain);

            CHECK(end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL,
                  "worker frame ends");
        }
        memset(pixels, 0, sizeof(pixels));
        memset(&rb, 0, sizeof(rb));
        CHECK(lc_image_readback(image, &rb, pixels, sizeof(pixels),
                                NULL) == LC_SUCCESS,
              "worker output reads back");
        CHECK(pixel_is(&pixels[((H / 2) * W + W / 2) * 4], 255, 0,
                       0),
              "worker count pixel is red");
        lc_command_list_destroy(list);
        lc_command_encoder_destroy(worker);
    }

    lc_buffer_destroy(plain);
    lc_buffer_destroy(ibo);
    lc_buffer_destroy(icommands);
    lc_buffer_destroy(count);
    lc_buffer_destroy(commands);
    lc_pipeline_destroy(pipeline);
    lc_render_target_destroy(target);
    lc_image_view_destroy(view);
    lc_image_destroy(image);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    printf("indirect-count: %d passed, %d failed\n", g_passed,
           g_failed);
    return g_failed ? 1 : 0;
}