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

/* Panels UI state (owned per-context; defined below — struct, not a
 * forward, because gui_inspector_widgets.cpp + gui_gizmo_overlay.cpp
 * + gui_panels.cpp all stage through it in one TU family).
 * int disciplines: ImGui wants bool* for window-open + checkbox
 * state; leg_ui stores int (C-ABI-friendly, MSVC-clean at /W3) and
 * bridges through locals at each call site (never casts int* to
 * bool* — that is UB and C2664 under MSVC). */
struct leg_ui {
    int show_hierarchy;
    int show_inspector;
    int show_assets;
    int show_console;
    int show_viewport;
    int show_toolbar;
    int show_status;
    int show_create_popup;
    int show_prefab_popup;
    char asset_search[256];
    int asset_type_filter; /* -1 = all, else led_project_asset_type */
    int asset_sort;        /* 0 path, 1 name, 2 type-then-path */
    int gizmo_mode;        /* led_gizmo_mode */
    int gizmo_space;       /* 0 world, 1 local (display only today) */
    int gizmo_snap_on;
    float gizmo_snap_step;
    int camera_speed;      /* scaled x100 (0.1x..10x) */
    int console_autoscroll;
    int console_level;     /* 0 all, 1 warn+, 2 errors */
    char create_name[128];
    int create_parent_to_selection;
    char prefab_path[1024];
    char status_text[256];
    int status_is_error;
    /* Inspector string staging (one row's edit buffer; committed on
     * Enter/focus-loss via led_write_property). */
    char string_stage[512];
    int staged_row;
    /* Gizmo drag staging (single active drag; screen px origin +
     * world anchor; committed through led_gizmo_begin/apply). */
    int gizmo_active;
    int gizmo_axis;
    float gizmo_start_px[2];
    float gizmo_start_world[3];
    float gizmo_plane_n[3];
    float gizmo_plane_d;
};

/* Viewport offscreen target (gui_viewport_tex.cpp; sized to the
 * viewport panel, zero-size-safe, swapchain-independent). COMPLETE
 * type here (not a forward): gui_draw.cpp validates viewport TexIDs
 * against the live set pointer and gui_panels.cpp samples the live
 * set for Image(), so both TUs need the field layout. Lifetime is
 * still owned by gui_viewport_tex.cpp (create/destroy here only). */
struct leg_viewport_target {
    uint32_t width;
    uint32_t height;
    lc_image *color;
    lc_image *depth;
    lc_image_view *color_view;
    lc_image_view *depth_view;
    lc_render_target *target;
    lc_binding_set *set;
    uint64_t generation; /* bumps every rebuild (stale Image guard) */
};

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
    /* Panels UI state (lazily created on first panels frame). */
    leg_ui *ui;
    /* Viewport target (lazily created on first viewport render;
     * NULL until then so headless contexts never touch the device). */
    leg_viewport_target *viewport_target;
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

/* Free the panels UI state (gui_panels.cpp; called from
 * leg_context_destroy). NULL-safe. */
void leg_ui_teardown(leg_context *ctx);

/* Status funnel (gui_panels.cpp owner; shared by widgets/gizmo). */
void leg_status(leg_context *ctx, const char *text, int error);

/* Panels frame entry (gui_panels.cpp; records all panels between
 * leg_frame_begin/end). NULL-safe. Hosts (the app + GPU proofs) call
 * this between begin/end; takes the session viewport + frame dt. */
void leg_panels_frame(leg_context *ctx, led_viewport *vp, float dt);

/* Viewport host hook (gui_panels.cpp owner): the panels frame caches
 * the viewport pointer for the gizmo overlay/camera update. */
void leg_viewport_host_set(led_viewport *vp, float dt);

/* One inspector row -> typed widget (gui_inspector_widgets.cpp).
 * Returns 1 when the row drew (0 for NULL args). */
int leg_widget_row(leg_context *ctx, leg_ui *ui, int row_id,
                   const led_inspector_row *row, const le_object *obj);

/* Gizmo overlay (gui_gizmo_overlay.cpp): screen-space handles over
 * the viewport capture rect + camera input. Returns 1 when a drag
 * or camera capture is active (the viewport should skip click
 * selection), 0 otherwise. */
int leg_gizmo_overlay(leg_context *ctx, leg_ui *ui,
                      const ImVec2 *origin, const ImVec2 *size);

/* Viewport camera update (gui_gizmo_overlay.cpp owner). */
void leg_viewport_camera_update(leg_context *ctx, leg_ui *ui,
                                led_viewport *vp, int hovered,
                                float dt);

/* Last record failure step (gui_draw.cpp owner; 0 = none/last-OK;
 * 1 gpu-ensure, 2 font-sync, 3 budget, 4 vtx buffer, 5 idx buffer,
 * 6 vtx upload, 7 idx upload, 8 bind pipeline, 9 bind vtx, 10 bind
 * idx, 11 push, 12 sampler set, 13 per-draw, 14 bad input). Test +
 * app diagnostics (never silent on GPU failure). */
int leg_record_step_last(void);

/* Viewport bridge (gui_viewport_tex.cpp): render the session world
 * into the panel-sized target + return the ImGui TexID for the panel
 * Image (ImTextureID_Invalid when nothing to show). enc must have NO
 * open pass. */
ImTextureID leg_viewport_render(leg_context *ctx,
                                leg_viewport_target *vt,
                                lc_command_encoder *enc,
                                uint32_t w, uint32_t h);

/* Destroy a viewport target + null it (gui_viewport_tex.cpp;
 * NULL-safe; views/target/set before images). */
void leg_viewport_target_destroy(leg_viewport_target **vt);

#endif /* LUMA_EDITOR_GUI_INTERNAL_H */
