/* Luma Editor internals (never public; public luma_editor.h +
 * luma_engine.h + luma_renderer.h + lumac.h only — same audit rule as
 * the engine: no backend, platform, internal, or Lua spellings). */

#ifndef LUMA_EDITOR_INTERNAL_H
#define LUMA_EDITOR_INTERNAL_H

#include <stdint.h>

#include "luma_editor/luma_editor.h"

/* One undoable history entry: the command as executed + its inverse
 * payload (before-bytes) + redo payload (after-bytes). Commands own
 * heap snapshots for subtree delete (serialized node arrays). */
typedef struct led_history_entry {
    led_command command;
    /* Inverse payload for undo. */
    uint8_t before_bytes[512];
    uint32_t before_size;
    le_object before_parent;
    int has_before_parent;
    /* Redo payload (== command.after for value kinds). */
    uint8_t after_bytes[512];
    uint32_t after_size;
    /* DELETE snapshots: serialized subtree (see editor_commands.c). */
    uint8_t *subtree_blob;
    uint32_t subtree_size;
    /* CREATE undo support: handle created by execute. */
    le_object created;
    int has_created;
    /* Timestamp (ms, editor clock) for coalescing. */
    uint64_t stamp_ms;
} led_history_entry;

typedef struct led_console_entry {
    led_log_level level;
    char tag[64];
    char message[256];
} led_console_entry;

struct led_session {
    /* Borrowed (host-owned; never destroyed here). */
    le_engine *engine;
    le_world *edit_world;
    int attached;
    /* Selection. */
    le_object selection[LED_SELECTION_MAX];
    uint32_t selection_count;
    /* History (bounded ring of entries). */
    led_history_entry *undo_stack;
    uint32_t undo_count;
    uint32_t undo_cap;
    led_history_entry *redo_stack;
    uint32_t redo_count;
    uint32_t redo_cap;
    uint32_t history_capacity;
    int coalesce_enabled;
    uint64_t coalesce_window_ms;
    uint64_t commands_pushed;
    uint64_t commands_evicted;
    uint64_t undos;
    uint64_t redos;
    uint64_t coalesced;
    uint64_t editor_ms; /* monotonic editor clock for coalescing */
    /* Dirty + scene path. */
    int dirty;
    char scene_path[1024];
    int has_scene_path;
    uint64_t commands_executed;
    int last_engine_error;
    /* Hierarchy snapshot. */
    led_hierarchy_node *hier_nodes;
    uint32_t hier_count;
    uint32_t hier_cap;
    /* Inspector rows. */
    led_inspector_row inspector_rows[LED_INSPECTOR_MAX_ROWS];
    uint32_t inspector_count;
    /* Play mode. */
    int playing;
    le_world *play_world;
    le_asset play_scene;
    int has_play_scene;
    int edit_paused_before_play;
    le_object pre_play_selection[LED_SELECTION_MAX];
    uint32_t pre_play_selection_count;
    uint64_t play_ticks;
    double play_elapsed;
    int play_paused;
    /* Gizmo. */
    int gizmo_active;
    led_gizmo_mode gizmo_mode;
    int gizmo_axis;
    le_object gizmo_target;
    /* Console. */
    led_console_entry console_entries[LED_CONSOLE_CAPACITY];
    uint32_t console_count;
    uint32_t console_start;
    uint64_t console_dropped;
    /* Focus. */
    int text_field_focused;
};

uint64_t led_clock_ms(void);
void led_entry_free(led_history_entry *entry);
int led_is_attached(const led_session *s);

#endif /* LUMA_EDITOR_INTERNAL_H */
