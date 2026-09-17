/* Phase 33 GUI frame loop (isolated C++ over public C ABIs).
 *
 * Hosts the desktop frame: leg_frame_begin drains the lc_window queue
 * (each event mapped once into io) + sets display extent/dt and opens
 * the dockspace root; the HOST calls leg_panels_frame between
 * begin/end (viewport + hierarchy + inspector + assets + console +
 * status); leg_frame_end closes the root, renders ImDrawData into
 * stats (user callbacks are skipped, counted, reported — never
 * executed), and leaves GPU recording to gui_draw.cpp's walk (same
 * pass, blended pipeline, per-draw scissor).
 *
 * Event-queue contract (no double-consume, no leak): while a GUI
 * context is active the host must NOT drain lc_window_read_event
 * itself — leg_frame_begin drains exactly once per frame. Focus/
 * close/resize events are observed (minimized bookkeeping) but never
 * consumed as GUI input (leg_feed_event returns 0 for them); the
 * host observes them through its own frame-input + should_close
 * polling, NOT by re-draining the queue. leg_feed_event maps one
 * caller-owned value (synthetic/headless) and never drains.
 *
 * Engine-input contract (no leaks into gameplay): the host pumps
 * lc_poll_events() BEFORE leg_frame_begin (the drain ingests OS
 * events into the lc queue). leg_frame_begin then maps each event
 * into ImGui io AND the engine observes the same queue? NO — the
 * engine drains via le_engine_begin_frame -> le_input_poll_platform
 * (lc_window_read_event). To avoid double-consume, the app attaches
 * NO window to the engine (le_engine_attach_window is never called
 * for the editor window): gameplay input during Play comes from
 * le_input_inject_* (driven by viewport hover + io state), never
 * from the OS queue behind the GUI's back. Text fields therefore
 * never leak keystrokes into gameplay, and Play never starves the
 * GUI: one queue, one drainer (the GUI), explicit injection across.
 */

#include <cstring>

#include "gui_internal.h"

led_result leg_frame_begin(leg_context *context, lc_window *window,
                           const leg_frame_input *input) {
    if (context == NULL || window == NULL || input == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (context->imgui == NULL) {
        return LED_ERROR_UNAVAILABLE;
    }
    if (context->frame_open) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    ImGui::SetCurrentContext(context->imgui);
    {
        ImGuiIO &io = ImGui::GetIO();

        /* Display extent: zero = minimized (valid frame, no draw). */
        io.DisplaySize.x = (float)input->window_width;
        io.DisplaySize.y = (float)input->window_height;
        io.DisplayFramebufferScale.x = 1.0f;
        io.DisplayFramebufferScale.y = 1.0f;
        /* dt clamp (0, 0.25]: timers stay sane across hitches. */
        {
            float dt = input->delta_seconds;

            if (!(dt == dt) || dt <= 0.0f) {
                dt = 1.0f / 60.0f;
            }
            if (dt > 0.25f) {
                dt = 0.25f;
            }
            io.DeltaTime = dt;
        }
        /* Drain the window queue exactly once (each event -> io). */
        for (;;) {
            lc_window_event ev;

            memset(&ev, 0, sizeof(ev));
            if (lc_window_read_event(window, &ev) != LC_SUCCESS) {
                break;
            }
            if (ev.type == LC_EVENT_NONE) {
                break;
            }
            /* Focus loss releases held-state pressure: an OS-level
             * focus drop without matching key-ups would stick keys
             * (the old code documented the release but no-op'd it —
             * fixed in Phase 33V). */
            if (ev.type == LC_EVENT_FOCUS_LOST) {
                io.ClearInputKeys();
            }
            leg_map_one_for_frame(io, &ev);
        }
        /* Focus state is consumed above (ClearInputKeys on loss);
         * window_focused remains host-observed (minimize handling),
         * not GUI-consumed. */
        (void)input->window_focused;
    }
    context->frame_w = input->window_width;
    context->frame_h = input->window_height;
    context->frame_minimized =
        (input->window_width == 0 || input->window_height == 0) ? 1
                                                                : 0;
    context->frame_open = 1;
    /* Open a new ImGui frame. The dockspace root is submitted by the
     * panels frame itself (LEG_DOCK_ROOT inside the dock-host window
     * below the menu/toolbar chrome — see gui_panels.cpp), so the
     * loop never imposes a fullscreen dockspace that would fight the
     * fixed chrome windows. */
    if (!context->frame_minimized) {
        ImGui::NewFrame();
    }
    return LED_SUCCESS;
}

led_result leg_frame_end(leg_context *context) {
    if (context == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (context->imgui == NULL) {
        return LED_ERROR_UNAVAILABLE;
    }
    if (!context->frame_open) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    memset(&context->last_stats, 0, sizeof(context->last_stats));
    if (!context->frame_minimized) {
        ImGui::SetCurrentContext(context->imgui);
        ImGui::Render();
        {
            const ImDrawData *draw = ImGui::GetDrawData();

            if (draw != NULL && draw->Valid) {
                leg_draw_stats stats;

                memset(&stats, 0, sizeof(stats));
                stats.cmd_lists = (uint32_t)draw->CmdLists.Size;
                stats.vertices = (uint32_t)draw->TotalVtxCount;
                stats.indices = (uint32_t)draw->TotalIdxCount;
                {
                    int n = 0;

                    for (n = 0; n < draw->CmdLists.Size; n++) {
                        const ImDrawList *list = draw->CmdLists[n];
                        int c = 0;

                        if (list == NULL) {
                            continue;
                        }
                        for (c = 0; c < list->CmdBuffer.Size; c++) {
                            const ImDrawCmd *cmd =
                                &list->CmdBuffer[c];

                            if (cmd->UserCallback != NULL) {
                                /* Never executed (spec): skipped +
                                 * counted so a callback-heavy frame
                                 * is observable, never silent. */
                                stats.user_callbacks_skipped++;
                                continue;
                            }
                            stats.draw_cmds++;
                        }
                    }
                }
                {
                    uint64_t vb = 0;
                    uint64_t ib = 0;
                    int ov = 0;

                    leg_draw_budget(stats.vertices, stats.indices,
                                    &vb, &ib, &ov);
                    stats.bytes_estimate = vb + ib;
                    (void)ov;
                }
                context->last_stats = stats;
            }
        }
    }
    context->frame_open = 0;
    return LED_SUCCESS;
}
