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
    /* Phase 32: open project (singleton per session). */
    led_project *project;
};

/* Forward: session/project cross-references (project TUs use
 * the opaque names; full struct led_session is defined above). */
typedef struct led_session led_session;
typedef struct led_project led_project;
uint64_t led_clock_ms(void);
void led_entry_free(led_history_entry *entry);
int led_is_attached(const led_session *s);

/* ---- Phase 32: project system (project/ subdir) ---- */

#define LED_PROJECT_MAX_RECORDS ((uint32_t)131072)
#define LED_PROJECT_PATH_MAX 1024
#define LED_PROJECT_ABS_MAX 2048

typedef struct led_db_record {
    led_project_asset_id id;
    led_project_asset_type type;
    char source_path[LED_PROJECT_PATH_MAX];
    led_import_status status;
    uint64_t fp_size;
    uint64_t fp_hash;
    char importer[32];
    uint32_t importer_version;
    uint64_t settings_digest;
    led_project_asset_id *deps;
    uint32_t dep_count;
    uint32_t dep_cap;
    char **sub_keys;
    le_asset_id *sub_ids;
    uint32_t sub_count;
    uint32_t sub_cap;
    le_asset runtime_asset;
    int has_runtime_asset;
    le_asset_id runtime_id;
    int has_runtime_id;
    char diagnostic[256];
    int has_sidecar;
} led_db_record;

typedef struct led_browser_sel {
    led_project_asset_id ids[256];
    uint32_t count;
} led_browser_sel;

struct led_project {
    char root[LED_PROJECT_ABS_MAX]; /* absolute, normalized */
    char name[128];
    uint32_t format_version;
    char startup_scene[LED_PROJECT_PATH_MAX];
    int has_startup_scene;
    char asset_roots[8][LED_PROJECT_PATH_MAX];
    uint32_t asset_root_count;
    uint32_t window_w;
    uint32_t window_h;
    char window_title[128];
    /* Database (UUID hash + path hash indexes over records). */
    led_db_record *records;
    uint32_t record_count;
    uint32_t record_cap;
    uint32_t *by_id;   /* open-addressing UUID -> index+1 (0 = empty) */
    uint32_t id_cap;
    uint32_t *by_path; /* open-addressing path-hash -> index+1 */
    uint32_t path_cap;
    uint64_t scan_seq;
    /* Browser model. */
    led_browser_folder *folders;
    uint32_t folder_count;
    uint32_t folder_cap;
    uint32_t *view_indices; /* record indices in view order */
    uint32_t view_count;
    uint32_t view_cap;
    led_project_asset_type view_filter;
    char view_search[256];
    int view_sort; /* 0 path, 1 name, 2 type-then-path */
    led_browser_sel browser_sel;
    led_inspector_row browser_inspector[64];
    uint32_t browser_inspector_count;
    /* Import queue counters. */
    uint32_t imp_pending;
    uint32_t imp_running;
    uint32_t imp_completed;
    uint32_t imp_failed;
    /* Drag state. */
    int drag_active;
    led_drag_payload drag_payload;
    /* Session link (borrowed engine for imports). */
    led_session *session;
};

void led_project_free_db(led_project *p);
int led_project_id_equal(const led_project_asset_id *a,
                         const led_project_asset_id *b);
void led_project_id_to_hex(const led_project_asset_id *id,
                           char out_hex[33]);
int led_project_id_from_hex(const char *hex,
                            led_project_asset_id *out_id);
void led_project_id_mint(led_project_asset_id *out_id);
uint64_t led_fnv1a64(const void *bytes, size_t size);

/* Cross-TU project helpers (asset_db.c owns sidecar + DB index;
 * project_scan.c owns scan; import_impl.c owns importer identity).
 * Declared here so every project TU shares one contract (no drift
 * between sibling-TU extern spellings). */
void led_sidecar_write_pub(led_project *p,
                           const led_db_record *r);
int led_sidecar_read_pub(led_project *p, const char *rel,
                         led_db_record *r);
int led_record_cmp_pub(const void *a, const void *b);
int led_db_rebuild(led_project *p);
int led_scan_reconcile(led_project *p, const char *rel,
                       const char *abs, led_scan_stats *st);
const char *led_importer_id_for(led_project_asset_type type);
uint32_t led_importer_version_for(led_project_asset_type type);
uint64_t led_import_settings_digest(led_project_asset_type type);
/* Canonical-text fingerprint shared by scan (asset_db.c) and the
 * post-import refresh (import_impl.c): LF vs CRLF checkouts of
 * scene/prefab/lua/project/sidecar sources hash identically
 * (bare CR stripped); binary formats hash exact bytes. */
int led_fingerprint_bytes_pub(const char *abs_path,
                              const char *rel,
                              uint64_t *out_size,
                              uint64_t *out_hash);
led_result led_import_one_record(led_session *s, uint32_t idx);
uint32_t led_browser_refresh(led_session *session);

#endif /* LUMA_EDITOR_INTERNAL_H */
