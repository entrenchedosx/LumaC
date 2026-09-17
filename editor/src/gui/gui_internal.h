/* Phase 33 GUI internals (C++-only; never public).
 *
 * This header is included ONLY by editor/src/gui/*.cpp. It is the one
 * place allowed to include imgui.h. It must never include Vulkan,
 * Win32/X11, engine/renderer internals, or Lua headers (enforced at
 * configure time by the GUI-confinement audit in
 * editor/CMakeLists.txt).
 */

#ifndef LUMA_EDITOR_GUI_INTERNAL_H
#define LUMA_EDITOR_GUI_INTERNAL_H

/* Public C ABIs this layer builds on (one-way dependency: none of
 * these headers names this layer back). */
#include <lumac/lumac.h>
#include <luma_engine/luma_engine.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_editor/luma_editor.h>

/* Dear ImGui (vendored docking branch; see third_party/imgui). */
#include "imgui.h"

#include <cstddef>
#include <cstdint>

/* Forward: GPU device objects owned per-context (gui_draw.cpp). */
struct leg_gpu;

/* Table size: 63 texture slots + 1 reserved entry (shared sampler
 * set cache). Texture allocation/draws never use the last entry. */
#define LEG_TEX_MAX 64u

/* Opaque GUI context, completed in gui_context.cpp. The C ABI
 * (leg_*) in luma_editor.h borrows session/device/window handles
 * (never owns); ImGui owns its context + font atlas pixels. */
struct leg_context {
    led_session *session;
    lc_device *device;
    ImGuiContext *imgui;
    char ini_path[1024];
    int has_ini_path;
    int frame_open;
    uint32_t frame_w;
    uint32_t frame_h;
    int frame_minimized;
    /* Draw-walk budget report (last leg_frame_end walk). */
    leg_draw_stats last_stats;
    /* GPU bridge (lazily created on first GPU use; NULL until then
     * so headless contexts never touch the device). */
    leg_gpu *gpu;
};

/* Map one event value into io (gui_input.cpp; shared by the
 * headless leg_feed_event probe and the frame drain). Returns 1 when
 * consumed as GUI input, 0 when ignored. Never drains any queue. */
int leg_map_one_for_frame(ImGuiIO &io, const lc_window_event *event);

/* Sync ImTextureData requests into the GPU texture table
 * (gui_draw.cpp; owned by the draw bridge, walked from gui_font.cpp
 * documentation). Returns 1 on success, 0 on table-full/upload
 * failure (caller skips drawing that frame, keeps prior atlas). */
int leg_font_sync(leg_context *ctx, struct leg_gpu *gpu);

/* Free the GPU bridge + struct (gui_draw.cpp; called from
 * leg_context_destroy while the device lives). NULL-safe. */
void leg_gpu_teardown(leg_context *ctx, struct leg_gpu *gpu);

/* Viewport offscreen target (gui_viewport_tex.cpp; sized to the
 * viewport panel, zero-size-safe, swapchain-independent). */
struct leg_viewport_target;

#endif /* LUMA_EDITOR_GUI_INTERNAL_H */
