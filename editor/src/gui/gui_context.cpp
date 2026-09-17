/* Phase 33 GUI context (isolated C++ over public C ABIs).
 *
 * Owns the Dear ImGui context for the Luma Editor desktop app:
 * create/destroy, per-frame begin/end, docking enablement, INI
 * separation (layout state never touches .luscene/prefab files).
 *
 * Confinement: this TU may include imgui.h (see gui_internal.h).
 * It must never include Vulkan/Win32/X11 headers, engine or
 * renderer internals, or Lua headers — input arrives as
 * lc_window_event values, drawing leaves as walked ImDrawData fed
 * to public lc_* calls owned by gui_draw.cpp. No engine/renderer/
 * LumaC header here names this layer back (one-way dependency).
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "gui_internal.h"

/* ImDrawVert stride contract (pos 8 + uv 8 + col 4 = 20 bytes).
 * Proven at context creation with a compile-time + runtime assert
 * pair (a future ImGui stride change fails loudly, never silently
 * mis-uploads). */
static const size_t kGuiVertStride = 20;

#define LEG_GUI_MAX_DRAW_BYTES ((uint64_t)(64ull * 1024ull * 1024ull))

led_result leg_context_create(led_session *session,
                              lc_device *device,
                              leg_context **out_context) {
    leg_context *ctx = NULL;

    if (out_context != NULL) {
        *out_context = NULL;
    }
    if (session == NULL || device == NULL || out_context == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    /* Session must be attached (the GUI drives led_* on it). */
    {
        led_session_stats stats;

        memset(&stats, 0, sizeof(stats));
        led_session_get_stats(session, &stats);
        if (!stats.attached) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
    }
    if (sizeof(ImDrawVert) != kGuiVertStride) {
        return LED_ERROR_UNAVAILABLE;
    }
    ctx = new (std::nothrow) leg_context;
    if (ctx == NULL) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->session = session;
    ctx->device = device;
    ctx->imgui = ImGui::CreateContext();
    if (ctx->imgui == NULL) {
        delete ctx;
        return LED_ERROR_UNAVAILABLE;
    }
    ImGui::SetCurrentContext(ctx->imgui);
    {
        ImGuiIO &io = ImGui::GetIO();

        /* Upstream docking (no custom docking engine, per spec). */
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        /* The GUI owns its renderer bridge (custom LumaC walk): tell
         * ImGui we handle vertex offsets + incremental textures. */
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendRendererName = "luma_gui_lumac";
        /* Layout INI separation: never beside scenes/prefabs. */
        io.IniFilename = NULL;
    }
    *out_context = ctx;
    return LED_SUCCESS;
}

void leg_context_destroy(leg_context *context) {
    if (context == NULL) {
        return;
    }
    /* GPU bridge first (frees lc_* objects while the device lives;
     * declared in gui_draw.cpp, defined per-TU to keep this file
     * device-free). */
    if (context->gpu != NULL) {
        leg_gpu_teardown(context, context->gpu);
        context->gpu = NULL;
    }
    /* Panels UI state (no device objects; order vs GPU irrelevant). */
    if (context->ui != NULL) {
        leg_ui_teardown(context);
    }
    /* Viewport target (lc_* objects; freed while the device lives). */
    if (context->viewport_target != NULL) {
        leg_viewport_target_destroy(&context->viewport_target);
    }
    if (context->imgui != NULL) {
        ImGui::SetCurrentContext(context->imgui);
        ImGui::DestroyContext(context->imgui);
        context->imgui = NULL;
    }
    context->session = NULL;
    context->device = NULL;
    delete context;
}

led_result leg_set_ini_path(leg_context *context, const char *path) {
    if (context == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (path == NULL || path[0] == '\0') {
        context->has_ini_path = 0;
        context->ini_path[0] = '\0';
        return LED_SUCCESS;
    }
    if (strlen(path) >= sizeof(context->ini_path)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    strncpy(context->ini_path, path, sizeof(context->ini_path) - 1);
    context->ini_path[sizeof(context->ini_path) - 1] = '\0';
    context->has_ini_path = 1;
    return LED_SUCCESS;
}

void leg_draw_get_stats(const leg_context *context,
                        leg_draw_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (context == NULL) {
        return;
    }
    *out_stats = context->last_stats;
}

/* ClipRect (DisplayPos-relative float px) -> lc_scissor_rect math.
 * Floor mins / ceil maxes / intersect with the pass extent. Empty
 * (inverted, zero-pass, NaN/Inf, fully-offscreen) -> 0 + zeroed. */
int leg_clip_to_scissor(float clip_min_x, float clip_min_y,
                        float clip_max_x, float clip_max_y,
                        uint32_t pass_w, uint32_t pass_h,
                        lc_scissor_rect *out_rect) {
    long x0 = 0;
    long y0 = 0;
    long x1 = 0;
    long y1 = 0;

    /* Bit-pattern NaN/Inf screen (MSVC /fp:fast may fold
     * fpclassify/isnan into constants; integer bit tests are never
     * folded). IEEE-754 binary32: exponent all-ones (0x7F800000)
     * means Inf (mantissa 0) or NaN (mantissa != 0). */
    {
        const float vals[4] = { clip_min_x, clip_min_y, clip_max_x,
                                clip_max_y };
        int k = 0;

        for (k = 0; k < 4; k++) {
            uint32_t bits = 0;

            memcpy(&bits, &vals[k], sizeof(bits));
            if ((bits & 0x7F800000u) == 0x7F800000u) {
                if (out_rect != NULL) {
                    memset(out_rect, 0, sizeof(*out_rect));
                }
                return 0;
            }
        }
    }
    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (pass_w == 0 || pass_h == 0) {
        return 0;
    }
    if (clip_min_x > 1e9f || clip_min_x < -1e9f ||
        clip_min_y > 1e9f || clip_min_y < -1e9f ||
        clip_max_x > 1e9f || clip_max_x < -1e9f ||
        clip_max_y > 1e9f || clip_max_y < -1e9f) {
        return 0;
    }
    /* Floor the mins, ceil the maxes (cover every touched pixel). */
    x0 = (long)std::floor(clip_min_x);
    y0 = (long)std::floor(clip_min_y);
    x1 = (long)std::ceil(clip_max_x);
    y1 = (long)std::ceil(clip_max_y);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (long)pass_w) {
        x1 = (long)pass_w;
    }
    if (y1 > (long)pass_h) {
        y1 = (long)pass_h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    if (out_rect != NULL) {
        out_rect->offset_x = (int32_t)x0;
        out_rect->offset_y = (int32_t)y0;
        out_rect->width = (uint32_t)(x1 - x0);
        out_rect->height = (uint32_t)(y1 - y0);
    }
    return 1;
}

/* ImDrawVert 20B / 16-bit index 2B budget probe. Caps at 64 MiB per
 * buffer; overflow reports the capped value + nonzero flag. */
uint64_t leg_draw_budget(uint32_t vertex_count,
                         uint32_t index_count,
                         uint64_t *out_vertex_bytes,
                         uint64_t *out_index_bytes,
                         int *out_overflow) {
    uint64_t vb = (uint64_t)vertex_count * kGuiVertStride;
    uint64_t ib = (uint64_t)index_count * 2u;
    int ov = 0;

    if (vb > LEG_GUI_MAX_DRAW_BYTES) {
        vb = LEG_GUI_MAX_DRAW_BYTES;
        ov = 1;
    }
    if (ib > LEG_GUI_MAX_DRAW_BYTES) {
        ib = LEG_GUI_MAX_DRAW_BYTES;
        ov = 1;
    }
    if (out_vertex_bytes != NULL) {
        *out_vertex_bytes = vb;
    }
    if (out_index_bytes != NULL) {
        *out_index_bytes = ib;
    }
    if (out_overflow != NULL) {
        *out_overflow = ov;
    }
    return vb;
}
