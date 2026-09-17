#ifndef LUMA_EDITOR_H
#define LUMA_EDITOR_H

/*
 * Luma Editor foundation (Phase 31): Godot-like editor architectural
 * foundation over the PUBLIC Luma Engine + Renderer + LumaC APIs.
 *
 * Layering (strict):
 *   editor -> engine -> assets -> renderer -> LumaC.
 * The editor never includes engine/renderer internals and never names
 * backend symbols; reflection routes through validated engine APIs
 * (le_object_is_alive, le_object_has_component, per-component getters).
 *
 * Split:
 * - EditorCore (headless): session, selection, commands, undo/redo
 *   history, dirty tracking, scene open/save, play-mode controller.
 *   Zero window/renderer calls; operates on borrowed le_engine /
 *   le_world handles (the host owns engine/world lifetime).
 * - EditorGUI models (headless plain data, Phase-32-renderable):
 *   hierarchy snapshot, inspector rows, viewport math, gizmo intents,
 *   console ring, shortcut table. A future GUI renders them; every
 *   model refreshes via led_*_refresh() and is unit-tested headless.
 *
 * Identity: selection/commands/instances name objects by le_object
 * handles validated with le_object_is_alive on every read path. A
 * stale handle (right index, wrong generation) fails safely and never
 * addresses a different live object. Play mode never runs gameplay on
 * the edit world: enter captures edit -> instantiates a fresh runtime
 * world; ticks step ONLY the runtime world; exit destroys it and the
 * edit world is byte-identical to pre-play (canonical oracle).
 *
 * Threading: single-threaded on the world's owning thread (same
 * contract as the engine). No thread safety is claimed.
 *
 * Conventions mirror the engine: every fallible function returns
 * led_result; NULL-safe; output pointers cleared/zeroed on failure
 * where documented; value getters zero-fill on bad input.
 */

#include <stddef.h>
#include <stdint.h>

#include <luma_engine/luma_engine.h>
#include <luma_renderer/luma_renderer.h>

/* Symbol visibility (mirrors LE_API; static builds expand empty). */
#if defined(_WIN32) || defined(_WIN64)
    #if defined(LUMA_EDITOR_BUILD_SHARED)
        #if defined(LUMA_EDITOR_EXPORTS)
            #define LED_API __declspec(dllexport)
        #else
            #define LED_API __declspec(dllimport)
        #endif
    #else
        #define LED_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define LED_API __attribute__((visibility("default")))
    #else
        #define LED_API
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Editor result codes (le_result values never leak; engine failures
 * surface as LED_ERROR_ENGINE with the code preserved in stats and
 * the console). Stable contract, safe to switch on. */
typedef enum led_result {
    LED_SUCCESS = 0,
    LED_ERROR_INVALID_ARGUMENT = 1001,
    LED_ERROR_STALE_HANDLE = 1002,
    LED_ERROR_WRONG_WORLD = 1003,
    LED_ERROR_NOT_ATTACHED = 1004,
    LED_ERROR_NO_EDIT_WORLD = 1005,
    LED_ERROR_ALREADY_PLAYING = 1006,
    LED_ERROR_NOT_PLAYING = 1007,
    LED_ERROR_PLAY_FAILED = 1008,
    LED_ERROR_VALIDATION = 1009,
    LED_ERROR_OUT_OF_MEMORY = 1010,
    LED_ERROR_OVERFLOW = 1011,
    LED_ERROR_UNAVAILABLE = 1012,
    LED_ERROR_IO = 1013,
    LED_ERROR_PARSE = 1014,
    LED_ERROR_ENGINE = 1015
} led_result;

/* Opaque editor session. Never dereference; use the API below. */
typedef struct led_session led_session;

/* ------------------------------------------------------------------
 * Session (EditorCore lifetime; borrows engine + edit world).
 * ------------------------------------------------------------------ */

/** Create an editor session (headless; no window/renderer needed).
 *  The session borrows nothing yet — attach() binds engine/world. */
LED_API led_result led_session_create(led_session **out_session);

/** Destroy a session (exits play mode first if active, freeing the
 *  runtime world; borrowed engine/world handles are untouched).
 *  NULL-safe. */
LED_API void led_session_destroy(led_session *session);

/** Attach engine + edit world (both borrowed, must outlive the
 *  session). Re-attach rebinds (clears selection/history/play).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (NULL args),
 *          LED_ERROR_OUT_OF_MEMORY. */
LED_API led_result led_session_attach(led_session *session,
                                      le_engine *engine,
                                      le_world *edit_world);

/** Detach (clears selection/history, exits play). Borrowed handles
 *  are NOT destroyed. NULL-safe no-op. */
LED_API void led_session_detach(led_session *session);

/** Borrowed handles (NULL when detached / for NULL session). */
LED_API le_engine *led_session_get_engine(const led_session *session);
LED_API le_world *led_session_get_edit_world(const led_session *session);

/** Advance editor housekeeping by dt seconds (console aging is
 *  future work; currently validates attachment and prunes stale
 *  selection). Edit-world scripts NEVER run here (matrices-only
 *  refresh so the outliner stays valid). */
LED_API led_result led_session_tick(led_session *session, float dt);

/** Plain-data session stats (zeros for NULL; out may be NULL). */
typedef struct led_session_stats {
    int attached;
    int playing;
    int edit_paused_before_play;
    uint32_t selection_count;
    uint32_t undo_depth;
    uint32_t redo_depth;
    int dirty;
    uint64_t commands_executed;
    uint64_t play_ticks;
    int last_engine_error;
} led_session_stats;

LED_API void led_session_get_stats(const led_session *session,
                                   led_session_stats *out_stats);

/* ------------------------------------------------------------------
 * Selection (ordered unique le_object set; liveness-filtered).
 * View state: changes never mark the scene dirty.
 * ------------------------------------------------------------------ */

#define LED_SELECTION_MAX ((uint32_t)256)

/** Replace the selection (NULL/empty clears). Stale handles are
 *  filtered at read time, never stored destructively. */
LED_API led_result led_selection_set(led_session *session,
                                     const le_object *objects,
                                     uint32_t count);
LED_API led_result led_selection_add(led_session *session,
                                     const le_object *object);
LED_API led_result led_selection_remove(led_session *session,
                                        const le_object *object);
LED_API led_result led_selection_toggle(led_session *session,
                                        const le_object *object);
LED_API led_result led_selection_clear(led_session *session);
/** Nonzero when the (live) object is selected. */
LED_API int led_selection_contains(led_session *session,
                                   const le_object *object);
/** Copy out live selection (ascending-slot order; counting query
 *  when out is NULL). Returns live count. */
LED_API uint32_t led_selection_get(led_session *session,
                                   le_object *out_objects,
                                   uint32_t capacity);
/** Drop stale/slot-reused handles now. Returns survivors. */
LED_API uint32_t led_selection_prune(led_session *session);
/** Select the object's whole subtree (object + descendants). */
LED_API led_result led_selection_select_subtree(led_session *session,
                                                const le_object *object);
/** Select first live object with an exact name match. */
LED_API led_result led_selection_select_by_name(led_session *session,
                                                const char *name);

/* ------------------------------------------------------------------
 * Reflection (editor-side static tables over validated engine APIs).
 * ------------------------------------------------------------------ */

/** Reflected property data types. */
typedef enum led_data_type {
    LED_DATA_BOOL = 0,
    LED_DATA_INT = 1,
    LED_DATA_UINT = 2,
    LED_DATA_FLOAT = 3,
    LED_DATA_VEC3 = 4,
    LED_DATA_QUAT = 5,
    LED_DATA_EULER_DEG = 6, /* inspector mirror of QUAT (see Euler policy) */
    LED_DATA_STRING = 7,
    LED_DATA_ENUM = 8,
    LED_DATA_ASSET_ID = 9, /* persistent le_asset_id hex (read-only) */
    LED_DATA_COLOR3 = 10,
    LED_DATA_UNAVAILABLE = 11 /* known but not representable (documented) */
} led_data_type;

/** One reflected property (static const storage; never free). */
typedef struct led_property_desc {
    const char *path;        /* "transform.position", "camera.fov_y_deg" ... */
    const char *label;       /* human label ("FOV Y (deg)") */
    le_component_type component;
    led_data_type type;
    uint32_t index;          /* channel index (script props use 0..n) */
    float min_value;
    float max_value;
    int has_range;
    const char *enum_labels; /* semicolon-separated, ENUM only (may be NULL) */
    int read_only;
} led_property_desc;

/** Component coverage for one object (presence bits). */
typedef struct led_object_schema {
    le_object handle;
    int alive;
    uint32_t component_mask; /* 1u << le_component_type */
    uint32_t property_count;
} led_object_schema;

/** Describe one object: presence mask + property count (0/empty for
 *  NULL/stale input; out may be NULL). */
LED_API void led_describe_object(led_session *session,
                                 const le_object *object,
                                 led_object_schema *out_schema);

/** List property descriptors for one object (up to capacity;
 *  always reports full count in out_count; either out pointer may
 *  be NULL). Returns live property count. */
LED_API uint32_t led_list_properties(led_session *session,
                                     const le_object *object,
                                     const led_property_desc **out_props,
                                     uint32_t capacity,
                                     uint32_t *out_count);

/** Find one property descriptor by path (NULL when absent/stale). */
LED_API const led_property_desc *led_find_property(
    led_session *session, const le_object *object, const char *path);

/** Typed property value (plain data; strings borrowed where noted). */
typedef struct led_property_value {
    led_data_type type;
    int boolean;
    int64_t integer;
    uint64_t uinteger;
    double number;
    float vec3[3];
    float quat[4];
    char string_value[256];
    char asset_hex[33];
} led_property_value;

/** Read one property by path (1 when present+readable, 0 for
 *  NULL/stale/unknown; out may be NULL for a presence query). */
LED_API int led_read_property(led_session *session,
                              const le_object *object, const char *path,
                              led_property_value *out_value);

/** Write one property by path (type + range validated; Euler-deg
 *  writes convert to quaternion storage).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (bad path/type/
 *          range/non-finite), LED_ERROR_STALE_HANDLE,
 *          LED_ERROR_NOT_ATTACHED, LED_ERROR_ENGINE. */
LED_API led_result led_write_property(led_session *session,
                                      const le_object *object,
                                      const char *path,
                                      const led_property_value *value);

/* ------------------------------------------------------------------
 * Hierarchy model (plain-data snapshot for future outliner UI).
 * ------------------------------------------------------------------ */

/** One hierarchy row (handle + depth + borrowed name). */
typedef struct led_hierarchy_node {
    le_object handle;
    uint32_t depth;
    int has_children;
    const char *name;
} led_hierarchy_node;

/** Refresh the hierarchy snapshot (roots ascending + DFS). Returns
 *  node count (0 when detached). Snapshot storage is session-owned;
 *  row pointers stay valid until the next refresh/detach/destroy. */
LED_API uint32_t led_hierarchy_refresh(led_session *session);
/** Borrow the snapshot (count via led_hierarchy_refresh or
 *  led_hierarchy_count). NULL when empty/detached. */
LED_API const led_hierarchy_node *led_hierarchy_nodes(
    const led_session *session);
LED_API uint32_t led_hierarchy_count(const led_session *session);

/* ------------------------------------------------------------------
 * Inspector model (plain-data rows for future property UI).
 * ------------------------------------------------------------------ */

#define LED_INSPECTOR_MAX_ROWS ((uint32_t)256)

/** One inspector row (descriptor + current value snapshot). */
typedef struct led_inspector_row {
    led_property_desc desc;
    led_property_value value;
    int has_value;
} led_inspector_row;

/** Build inspector rows for one object (0/empty for NULL/stale).
 *  Returns row count; rows are session-owned until next inspect/
 *  refresh/detach/destroy. */
LED_API uint32_t led_inspect(led_session *session,
                             const le_object *object);
LED_API const led_inspector_row *led_inspector_rows(
    const led_session *session);
LED_API uint32_t led_inspector_count(const led_session *session);

/* ------------------------------------------------------------------
 * Commands + undo/redo (GUI-independent, MCP-ready plain structs).
 * Every mutation flows UI -> led_command -> engine -> history -> dirty.
 * ------------------------------------------------------------------ */

/** Project manifest format version (1 in Phase 32; open rejects
 *  anything else with LED_ERROR_PARSE). */
#define LED_PROJECT_FORMAT_VERSION ((uint32_t)1)

/** Project asset types (stable contract; append-only). */
typedef enum led_project_asset_type {
    LED_PROJECT_ASSET_UNKNOWN = 0,
    LED_PROJECT_ASSET_MODEL = 1,   /* .glb/.gltf source */
    LED_PROJECT_ASSET_TEXTURE = 2, /* .png/.jpg/.jpeg source */
    LED_PROJECT_ASSET_SCRIPT = 3,  /* .lua source */
    LED_PROJECT_ASSET_SCENE = 4,   /* .luma_scene source */
    LED_PROJECT_ASSET_PREFAB = 5,  /* .luprefab source */
    LED_PROJECT_ASSET_MESH = 6,    /* glTF sub-asset */
    LED_PROJECT_ASSET_MATERIAL = 7,/* glTF sub-asset */
    LED_PROJECT_ASSET_SKELETON = 8,/* glTF sub-asset */
    LED_PROJECT_ASSET_CLIP = 9,    /* glTF sub-asset */
    LED_PROJECT_ASSET_TYPE_COUNT = 10
} led_project_asset_type;

/** Import status (explicit state, never inferred from UI). */
typedef enum led_import_status {
    LED_IMPORT_UNIMPORTED = 0,
    LED_IMPORT_READY = 1,
    LED_IMPORT_STALE = 2,
    LED_IMPORT_FAILED = 3,
    LED_IMPORT_MISSING = 4,
    LED_IMPORT_UNSUPPORTED = 5
} led_import_status;

/** Project asset UUID (authoring identity; printed/persisted as 32
 *  lowercase hex digits, same spelling as le_asset_id). */
typedef struct led_project_asset_id {
    uint64_t hi;
    uint64_t lo;
} led_project_asset_id;

/** Opaque project handle. Never dereference; use the API below.
 *  At most one project is open per led_session (singleton policy);
 *  open/close/switch in the same process is supported. */
typedef struct led_project led_project;
/** Command kinds (stable contract; safe to persist in macros).
 *  Phase 32 appends prefab kinds (values stable, never reordered). */
typedef enum led_command_kind {
    LED_CMD_CREATE = 0,
    LED_CMD_DELETE = 1,
    LED_CMD_SET_NAME = 2,
    LED_CMD_SET_ENABLED = 3,
    LED_CMD_SET_POSITION = 4,
    LED_CMD_SET_ROTATION = 5,
    LED_CMD_SET_SCALE = 6,
    LED_CMD_SET_PARENT = 7,
    LED_CMD_REPARENT = 8,
    LED_CMD_ADD_COMPONENT = 9,
    LED_CMD_REMOVE_COMPONENT = 10,
    LED_CMD_SET_SCRIPT_PROPERTY = 11,
    LED_CMD_SET_CAMERA = 12,
    LED_CMD_SET_LIGHT = 13,
    LED_CMD_SET_RIGID_BODY = 14,
    LED_CMD_SET_COLLIDER = 15,
    LED_CMD_SET_ANIMATOR = 16,
    LED_CMD_SET_CHARACTER = 17,
    /* Phase 32: prefab + typed asset assignment (appended). */
    LED_CMD_INSTANTIATE_PREFAB = 18,
    LED_CMD_CREATE_PREFAB = 19,
    LED_CMD_ASSIGN_ASSET = 20,
    LED_CMD_KIND_COUNT = 21
} led_command_kind;

/** Prefab payload on led_command (valid when kind is a prefab
 *  kind): prefab registry handle + project UUID + instance root
 *  (for undo) + assign target/role. */
typedef struct led_prefab_command {
    le_asset prefab_asset;
    led_project_asset_id project_id;
    le_object instance_root;
    int has_instance_root;
    le_object assign_target;
    led_project_asset_type assign_role; /* MATERIAL or SCRIPT */
} led_prefab_command;

/** Command payload (plain values; strings copied into fixed buffers;
 *  component bytes are full le_*_desc snapshots). */
typedef struct led_command {
    led_command_kind kind;
    char label[96];
    le_object target;      /* object the command applies to */
    le_object parent;      /* SET_PARENT/REPARENT/CREATE parent (or INVALID) */
    int has_parent;
    int reparent_mode;     /* LED_REPARENT_* (REPARENT only) */
    char name_value[128];  /* SET_NAME */
    int enabled_value;     /* SET_ENABLED */
    float vec_value[4];    /* TRS (rotation uses xyzw) */
    le_component_type component; /* ADD/REMOVE_COMPONENT + SET_* kinds */
    uint8_t comp_bytes[512];     /* component desc snapshot (validated size) */
    uint32_t comp_size;
    le_script_property script_prop; /* SET_SCRIPT_PROPERTY (after value) */
    led_prefab_command prefab; /* prefab kinds (INSTANTIATE/CREATE/ASSIGN) */
    uint32_t create_index_hint;     /* CREATE: reserved, must be 0 */
} led_command;

/** Reparent policy mirror (matches le_reparent_mode values). */
typedef enum led_reparent_mode {
    LED_REPARENT_KEEP_LOCAL = 0,
    LED_REPARENT_KEEP_WORLD = 1
} led_reparent_mode;

/** Execute a command on the edit world (validates first; engine
 *  failure pushes NOTHING — history and dirty stay untouched).
 *  Success pushes the inverse onto undo, clears redo, sets dirty.
 *  @return LED_SUCCESS / INVALID_ARGUMENT / STALE_HANDLE /
 *          NOT_ATTACHED / ENGINE / OUT_OF_MEMORY / OVERFLOW. */
LED_API led_result led_execute(led_session *session,
                               const led_command *command);

/** Undo last (0 when empty; out_undo may be NULL for a no-op
 *  query... actually performs the undo). Returns 1 when something
 *  was undone, 0 when the stack is empty. */
LED_API int led_undo(led_session *session);
/** Redo last undone (1/0, same contract). */
LED_API int led_redo(led_session *session);
/** Empty both stacks (counts toward evicted stats, never dirty). */
LED_API void led_history_clear(led_session *session);

/** Bounded-history tuning (defaults: cap 256, coalesce on, 500 ms).
 *  Cap 0 rejects (INVALID_ARGUMENT); cap is clamped to [1, 65536]. */
LED_API led_result led_history_set_capacity(led_session *session,
                                            uint32_t capacity);
LED_API led_result led_history_set_coalesce(led_session *session,
                                            int enabled,
                                            uint64_t window_ms);

/** Plain-data history stats (zeros for NULL; out may be NULL). */
typedef struct led_history_stats {
    uint32_t undo_depth;
    uint32_t redo_depth;
    uint32_t capacity;
    int coalesce_enabled;
    uint64_t commands_pushed;
    uint64_t commands_evicted;
    uint64_t undos;
    uint64_t redos;
    uint64_t coalesced;
    uint64_t bytes_estimate;
} led_history_stats;

LED_API void led_history_get_stats(const led_session *session,
                                   led_history_stats *out_stats);

/* ------------------------------------------------------------------
 * Scene I/O + dirty tracking.
 * ------------------------------------------------------------------ */

/** Dirty flag (1 when the edit world has unsaved command changes).
 *  Set ONLY by successful led_execute; cleared by save/load/new/
 *  revert. Undo/redo/failed-save never clear it. */
LED_API int led_is_dirty(const led_session *session);

/** New empty edit scene (destroys all edit-world objects via
 *  validated per-object destroy; clears selection/history; clears
 *  dirty). Fails when playing (stop first). */
LED_API led_result led_scene_new(led_session *session);
/** Open a scene file into the edit world (transactional: failed
 *  load preserves the edit world; clears selection/history/dirty
 *  on success only). Fails when playing. */
LED_API led_result led_scene_open(led_session *session,
                                  const char *path);
/** Save the edit world to path (capture -> serialize -> write;
 *  failed save KEEPS dirty). Clears dirty on success. Remembers
 *  the path for led_scene_save. */
LED_API led_result led_scene_save_as(led_session *session,
                                     const char *path);
/** Save to the remembered path (IO error when none yet). */
LED_API led_result led_scene_save(led_session *session);
/** Reload the remembered path (failed load preserves edit world).
 *  IO error when no path remembered. */
LED_API led_result led_scene_revert(led_session *session);
/** Borrow the remembered path ("" when none; NULL for NULL). */
LED_API const char *led_scene_get_path(const led_session *session);

/* ------------------------------------------------------------------
 * Play mode (edit <-> runtime isolation).
 * ------------------------------------------------------------------ */

/** Enter play: capture edit -> instantiate a FRESH runtime world on
 *  the same engine; pause edit; unpause runtime. Edit scripts NEVER
 *  run during play. Selection maps via persistent IDs best-effort.
 *  @return LED_SUCCESS, LED_ERROR_ALREADY_PLAYING, LED_ERROR_*
 *          (edit untouched, no runtime leaked, on failure). */
LED_API led_result led_play_enter(led_session *session);
/** Tick ONLY the runtime world (explicit dt; deterministic). Edit
 *  receives a matrices-only refresh. Fails when not playing. */
LED_API led_result led_play_tick(led_session *session, float dt);
/** Pause/resume the runtime world (edit stays paused throughout). */
LED_API led_result led_play_set_paused(led_session *session, int paused);
LED_API int led_play_is_paused(const led_session *session);
/** Single-step one fixed interval while paused (debug stepping). */
LED_API led_result led_play_step(led_session *session);
/** Exit play: destroy the runtime world, unpause edit, restore
 *  pre-play selection (pruned). Edit is byte-identical to pre-play
 *  (canonical capture oracle). */
LED_API led_result led_play_exit(led_session *session);
/** Nonzero while a runtime world is live. */
LED_API int led_is_playing(const led_session *session);
/** Borrow the runtime world (NULL when not playing). */
LED_API le_world *led_play_get_world(led_session *session);

/** Plain-data play stats (zeros for NULL; out may be NULL). */
typedef struct led_play_stats {
    int playing;
    int runtime_paused;
    uint64_t ticks;
    double runtime_elapsed;
    uint32_t runtime_objects;
    uint64_t edit_script_guard_ticks;
} led_play_stats;

LED_API void led_play_get_stats(const led_session *session,
                                led_play_stats *out_stats);

/* ------------------------------------------------------------------
 * Viewport (math only, headless; no renderer submission here).
 * ------------------------------------------------------------------ */

/** Orbit viewport state (plain values; host owns the struct). */
typedef struct led_viewport {
    uint32_t width;
    uint32_t height;
    float target[3];
    float yaw_rad;
    float pitch_rad;
    float distance;
    float fov_y_rad;
    float near_plane;
    float far_plane;
} led_viewport;

/** Fill orbit defaults (640x480, target origin, yaw 0, pitch -0.35,
 *  distance 8, 60 deg FOV, near 0.1, far 1000). NULL-safe no-op. */
LED_API void led_viewport_default(led_viewport *viewport);
/** Orbit deltas (yaw wraps, pitch clamps [-1.55, 1.55], distance
 *  clamps [0.05, 1e5]). NULL-safe no-op. */
LED_API void led_viewport_orbit(led_viewport *viewport, float dyaw,
                                float dpitch);
/** Dolly (multiplies distance; same clamp). Pan (moves target in
 *  the camera frame). NULL-safe no-ops. */
LED_API void led_viewport_dolly(led_viewport *viewport, float factor);
LED_API void led_viewport_pan(led_viewport *viewport, float dx, float dy);

/** Derive an lr_camera from viewport state (1 on success, 0 for
 *  NULL/bad state; out may be NULL for validation). */
LED_API int led_viewport_camera(const led_viewport *viewport,
                                lr_camera *out_camera);

/** World-space ray for a client pixel (origin top-left, +x right,
 *  +y down — LumaC convention). Returns 1 on success (out_origin +
 *  normalized out_dir), 0 for NULL/bad viewport. */
LED_API int led_viewport_ray(const led_viewport *viewport,
                             float pixel_x, float pixel_y,
                             float out_origin[3], float out_dir[3]);

/** Pick against EITHER world (edit default): physics raycast along
 *  the viewport ray (1 = hit with out_hit filled, 0 = none).
 *  Requires colliders (mesh-without-collider picking is documented
 *  unsupported). max_distance <= 0 selects far_plane. */
LED_API int led_viewport_pick(led_session *session,
                              const led_viewport *viewport,
                              float pixel_x, float pixel_y,
                              float max_distance, uint32_t layer_mask,
                              le_ray_hit *out_hit);
/** Pick + select (replaces selection on hit, clears on miss).
 *  Returns 1 on hit, 0 on miss/failure. */
LED_API int led_viewport_pick_select(led_session *session,
                                     const led_viewport *viewport,
                                     float pixel_x, float pixel_y);

/** World-space AABB (plain min/max; 1 when computable, 0 with
 *  zeroed out for empty/missing/stale/asset-backed). Pointer
 *  renderables only; asset-backed report LED-unavailable (never
 *  guessed). Mirrored bases expand conservatively. */
LED_API int led_compute_world_aabb(led_session *session,
                                   const le_object *object,
                                   float out_min[3], float out_max[3]);
/** Union AABB over the live selection (1/0, same contract). */
LED_API int led_selection_aabb(led_session *session, float out_min[3],
                               float out_max[3]);
/** Fit orbit distance to the selection (or whole scene when empty;
 *  0 when nothing frames). */
LED_API int led_frame_selection(led_session *session,
                                led_viewport *viewport);

/* ------------------------------------------------------------------
 * Gizmos (math-only intents; no rendering path in Phase 31).
 * ------------------------------------------------------------------ */

/** Gizmo modes + axes. */
typedef enum led_gizmo_mode {
    LED_GIZMO_TRANSLATE = 0,
    LED_GIZMO_ROTATE = 1,
    LED_GIZMO_SCALE = 2
} led_gizmo_mode;

/** One gizmo drag intent (plain values; apply via led_gizmo_apply
 *  which routes through a coalesced undoable command). */
typedef struct led_gizmo_drag {
    led_gizmo_mode mode;
    int axis; /* 0=x, 1=y, 2=z, 3=free-plane/xyz */
    float start_world[3];
    float current_world[3];
    float snap; /* 0 = off; translate meters, rotate degrees, scale frac */
} led_gizmo_drag;

/** Begin a drag for the current single selection (1/0). */
LED_API int led_gizmo_begin(led_session *session, led_gizmo_mode mode,
                            int axis);
/** Apply a drag delta as ONE coalesced undoable command. */
LED_API led_result led_gizmo_apply(led_session *session,
                                   const led_gizmo_drag *drag);
/** Preview line soup (editor-owned float buffer xyzxyz; counting
 *  query when out is NULL). Returns segment-float count. Covers
 *  selection AABB edges + axis rays for the active gizmo mode. */
LED_API uint32_t led_gizmo_lines(led_session *session,
                                 const led_viewport *viewport,
                                 float *out_xyz, uint32_t float_cap);

/* ------------------------------------------------------------------
 * Console (ring buffer) + last-error mirror.
 * ------------------------------------------------------------------ */

typedef enum led_log_level {
    LED_LOG_INFO = 0,
    LED_LOG_WARNING = 1,
    LED_LOG_ERROR = 2
} led_log_level;

#define LED_CONSOLE_CAPACITY ((uint32_t)1024)

/** Push a log entry (oldest drops past capacity, counted). */
LED_API led_result led_console_push(led_session *session,
                                    led_log_level level, const char *tag,
                                    const char *message);
/** Entry count (0 for NULL/detached... console lives on session). */
LED_API uint32_t led_console_count(const led_session *session);
/** Borrow entry i (0 = oldest; NULL for out-of-range/NULL). */
LED_API const char *led_console_message(const led_session *session,
                                       uint32_t index,
                                       led_log_level *out_level,
                                       const char **out_tag);
LED_API void led_console_clear(led_session *session);
/** Mirror the engine's last script error into the console (1 when
 *  an error was mirrored, 0 when none). */
LED_API int led_console_mirror_script_error(led_session *session);

/* ------------------------------------------------------------------
 * Shortcuts + focus policy (pure headless tables/flags).
 * ------------------------------------------------------------------ */

/** Editor actions (stable contract for future GUI/MCP binding). */
typedef enum led_action {
    LED_ACTION_UNDO = 0,
    LED_ACTION_REDO = 1,
    LED_ACTION_DUPLICATE = 2,
    LED_ACTION_DELETE = 3,
    LED_ACTION_PLAY = 4,
    LED_ACTION_STOP = 5,
    LED_ACTION_STEP = 6,
    LED_ACTION_SAVE = 7,
    LED_ACTION_FOCUS_SELECTION = 8,
    LED_ACTION_COUNT = 9
} led_action;

/** Match a key+mods to an action (1 + out when matched, 0 when
 *  not). Pure function (no session needed). */
LED_API int led_shortcut_match(le_key key, uint32_t mods,
                               led_action *out_action);
/** Focus scopes: viewport shortcuts suppressed while a text field
 *  owns focus (console/field). Headless flag on the session. */
LED_API void led_focus_set(led_session *session, int text_field_focused);
LED_API int led_focus_get(const led_session *session);
/** Dispatch one action (undo/redo/save/play/stop/step/duplicate/
 *  delete/focus hooks into the session + viewport where noted).
 *  Returns 1 when handled, 0 when not applicable. */
LED_API int led_dispatch_action(led_session *session, led_action action,
                                led_viewport *viewport);

/* ------------------------------------------------------------------
 * Editor project sidecar (self-contained versioned text; editor
 * state only — the scene itself travels via le_scene_* text).
 * ------------------------------------------------------------------ */

/** Save editor sidecar (scene path, history tuning, viewport).
 *  viewport may be NULL (skips the viewport line). */
LED_API led_result led_project_save_sidecar(
    led_session *session, const led_viewport *viewport,
    const char *path);
/* ------------------------------------------------------------------
 * Editor project sidecar (self-contained versioned text; editor
 * state only — the scene itself travels via le_scene_* text).
 * ------------------------------------------------------------------ */

/** Save editor sidecar (scene path, history tuning, viewport).
 *  viewport may be NULL (skips the viewport line). */
LED_API led_result led_project_save_sidecar(
    led_session *session, const led_viewport *viewport,
    const char *path);
/** Load editor sidecar (validates magic; unknown fields tolerated;
 *  applies tuning + viewport + remembered path, never opens the
 *  scene). viewport may be NULL (viewport line still parsed). */
LED_API led_result led_project_load_sidecar(led_session *session,
                                            led_viewport *viewport,
                                            const char *path);

/* ------------------------------------------------------------------
 * Luma Project system (Phase 32): portable project roots with a
 * small versioned manifest, a project asset database over
 * sidecar metadata, transactional import/reimport over existing
 * runtime pipelines, a headless asset browser model, and prefab
 * authoring. GUI-independent: every workflow below runs headless
 * (future MCP drives these same APIs; no protocol here).
 *
 * Identity stack (four concepts, never conflated):
 * - project asset ID (led_project_asset_id UUID): authoring
 *   identity, minted at discovery, stored in the sidecar. Stable
 *   across rename/move/reimport/cache-delete. Two copies of
 *   identical bytes get DISTINCT IDs.
 * - content fingerprint {size, FNV-1a-64}: change detection only.
 * - runtime le_asset handle: engine registry; reimport publishes a
 *   NEW handle (old one detectably stale), project ID unchanged.
 * - le_asset_id persistent ID: what scene/prefab files store; the
 *   DB bridges project-ID <-> le_asset_id <-> runtime handle.
 * ------------------------------------------------------------------ */


/* ---- project lifetime ---- */

/** Create a minimal project on disk (manifest + Assets/ + Scenes/).
 *  Fails when the directory exists and is nonempty (use open).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (NULL args),
 *          LED_ERROR_IO, LED_ERROR_OUT_OF_MEMORY. */
LED_API led_result led_project_create(const char *root_dir,
                                      const char *project_name);

/** Open a project (parse manifest -> scan sidecars -> build DB ->
 *  resolve startup scene). The project borrows the session's
 *  engine for imports (session must be attached). Reimport policy
 *  is lazy: open discovers + validates, imports on demand.
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT,
 *          LED_ERROR_NOT_ATTACHED, LED_ERROR_IO (missing
 *          manifest/unreadable root), LED_ERROR_PARSE (bad
 *          manifest/version/corrupt), LED_ERROR_OUT_OF_MEMORY. */
LED_API led_result led_project_open(led_session *session,
                                    const char *root_dir);

/** Close the open project (releases DB + project state; engine
 *  assets obey Phase 25 ownership — runtime handles stay live
 *  until unloaded). NULL-safe no-op. */
LED_API void led_project_close(led_session *session);

/** Nonzero while a project is open on this session. */
LED_API int led_project_is_open(const led_session *session);

/** Borrow the open project (NULL when none). */
LED_API led_project *led_project_get(led_session *session);

/** Plain-data project info (zeros/"" for NULL/closed). */
typedef struct led_project_info {
    char name[128];
    char root[1024];
    uint32_t format_version;
    char startup_scene[1024];
    int has_startup_scene;
    uint32_t asset_count;
    uint32_t ready_count;
    uint32_t stale_count;
    uint32_t failed_count;
    uint32_t missing_count;
} led_project_info;

LED_API void led_project_get_info(const led_session *session,
                                  led_project_info *out_info);

/* ---- paths (authoritative normalization + escape rejection) ---- */

/** Normalize a project-relative path in place discipline (out may
 *  alias nothing; out must hold 1024 bytes): `/` separators,
 *  collapsed duplicates, resolved `.`/`..`, no trailing slash,
 *  backslashes folded. Returns 1 on success, 0 for NULL/empty/
 *  overlong/unrepresentable input. Lexical only (no filesystem,
 *  no CWD dependence). */
LED_API int led_project_normalize(const char *path, char out[1024]);

/** Resolve a project-relative path against the open project root
 *  into an absolute OS path (out must hold 2048 bytes). Rejects
 *  escapes above the root (`..` breakout, absolute external
 *  paths, drive-absolute paths on Windows) with 0. Returns 1 on
 *  success. Never depends on the process CWD. */
LED_API int led_project_resolve(const led_session *session,
                                const char *rel_path,
                                char out_absolute[2048]);

/* ---- scan (explicit; no file watcher) ---- */

/** Scan the project (discover sources -> read sidecars -> update
 *  DB -> fingerprint compare -> mark STALE/MISSING; does NOT
 *  import). Incremental: unchanged projects import nothing and
 *  complete fast. Returns counts via out stats when non-NULL.
 *  @return LED_SUCCESS / NOT_ATTACHED / IO / OUT_OF_MEMORY. */
typedef struct led_scan_stats {
    uint32_t discovered;
    uint32_t added;
    uint32_t removed;
    uint32_t stale_marked;
    uint32_t missing_marked;
    uint32_t restored;
    uint32_t errors;
} led_scan_stats;

LED_API led_result led_project_scan(led_session *session,
                                    led_scan_stats *out_stats);

/** Reimport every STALE record (transactional per record; failed
 *  records keep last-known-good + FAILED diagnostics). Returns the
 *  number reimported-ok in out_ok when non-NULL. */
LED_API led_result led_project_reimport_all(led_session *session,
                                            uint32_t *out_ok);

/* ---- project filesystem operations (NOT scene undo) ---- */

/** Rename/move an asset within the project (source + sidecar move
 *  together; project ID preserved, references intact). Fails when
 *  the destination exists or escapes the root.
 *  @return LED_SUCCESS / INVALID_ARGUMENT / NOT_ATTACHED / IO /
 *          PARSE (DB conflict) / OUT_OF_MEMORY. */
LED_API led_result led_project_rename(led_session *session,
                                      const char *old_rel,
                                      const char *new_rel);

/** Delete a project asset (source + sidecar removed). Fails with
 *  LED_ERROR_VALIDATION + dependency listing in the console when
 *  referenced by scenes/prefabs/other assets (no force-delete in
 *  Phase 32). */
LED_API led_result led_project_delete(led_session *session,
                                      const char *rel_path);

/* ---- project asset database (GUI-independent) ---- */

/** One database record snapshot (plain data; strings copied into
 *  fixed buffers — no lifetime hazards). */
typedef struct led_asset_record {
    led_project_asset_id id;
    char id_hex[33];
    led_project_asset_type type;
    char source_path[1024]; /* project-relative, normalized */
    led_import_status status;
    uint64_t fingerprint_size;
    uint64_t fingerprint_hash;
    char importer[32]; /* "luma.gltf" etc; "" when undiscovered */
    uint32_t importer_version;
    uint64_t settings_digest;
    uint32_t dependency_count;
    uint32_t sub_asset_count;
    /* Stable sub-asset keys, parallel to the record's sub-ID table
     * (Phase 34A portable identity; up to 64 copied, count in
     * sub_asset_count — longer tables truncate the STRINGS only,
     * never the count). Empty when the record has no sub-assets. */
    char sub_keys[64][128];
    uint32_t sub_key_count;
    le_asset runtime_asset; /* INVALID when not imported */
    int has_runtime_asset;
    le_asset_id runtime_id; /* persistent engine ID; nil when none */
    int has_runtime_id;
    char diagnostic[256];
} led_asset_record;

/** Record count (0 for NULL/closed). */
LED_API uint32_t led_assetdb_count(const led_session *session);

/** Snapshot record i in deterministic (path-sorted) order (1 on
 *  success, 0 for out-of-range/NULL). */
LED_API int led_assetdb_get(const led_session *session, uint32_t index,
                            led_asset_record *out_record);

/** Look up by project UUID (1 + fill, 0 when absent). */
LED_API int led_assetdb_find_by_id(
    const led_session *session, const led_project_asset_id *id,
    led_asset_record *out_record);

/** Look up by project-relative path (normalized internally; 1/0). */
LED_API int led_assetdb_find_by_path(const led_session *session,
                                     const char *rel_path,
                                     led_asset_record *out_record);

/** Count records of one type (TYPE_COUNT counts all). */
LED_API uint32_t led_assetdb_filter(const led_session *session,
                                    led_project_asset_type type);

/** Search by name/path substring + optional type filter (writes up
 *  to capacity UUIDs in deterministic order; always reports the
 *  full count in out_count; either out pointer may be NULL).
 *  Case-insensitive substring; empty query matches all of the
 *  type. Returns full match count. */
LED_API uint32_t led_assetdb_search(
    const led_session *session, const char *query,
    led_project_asset_type type, led_project_asset_id *out_ids,
    uint32_t capacity, uint32_t *out_count);

/** Dependency list for one record (up to capacity UUIDs;
 *  counting query when out NULL). Returns full count. */
LED_API uint32_t led_assetdb_dependencies(
    const led_session *session, const led_project_asset_id *id,
    led_project_asset_id *out_ids, uint32_t capacity);

/** Reverse dependencies: records depending on id (same contract). */
LED_API uint32_t led_assetdb_dependents(
    const led_session *session, const led_project_asset_id *id,
    led_project_asset_id *out_ids, uint32_t capacity);

/** Plain-data DB stats (zeros for NULL; out may be NULL). */
typedef struct led_assetdb_stats {
    uint32_t records;
    uint32_t by_type[LED_PROJECT_ASSET_TYPE_COUNT];
    uint32_t by_status[6];
    uint64_t bytes_estimate;
} led_assetdb_stats;

LED_API void led_assetdb_get_stats(const led_session *session,
                                   led_assetdb_stats *out_stats);

/* ---- import / reimport (over existing runtime pipelines) ---- */

/** Importer identity + version table entry (static storage). */
typedef struct led_importer_info {
    char id[32]; /* "luma.gltf" */
    uint32_t version;
    const char *extensions; /* "; "-separated ("glb;gltf") */
} led_importer_info;

/** Importer count + lookup (pure; no session needed). */
LED_API uint32_t led_importer_count(void);
LED_API const led_importer_info *led_importer_at(uint32_t index);
LED_API const led_importer_info *led_importer_for_extension(
    const char *extension);

/** Import one source asset (discover -> import -> DB READY).
 *  Transactional: failure leaves any prior good state + FAILED
 *  diagnostics (never half-imported). Reimport while PLAY is
 *  rejected (conservative policy). */
LED_API led_result led_import_asset(led_session *session,
                                    const char *rel_path);

/** Reimport one record by project UUID (stale check: fingerprint
 *  OR settings OR importer-version change; forced when
 *  force != 0). Candidate-then-swap: new runtime handle published
 *  only after full validation; failure keeps the old asset live. */
LED_API led_result led_reimport_asset(
    led_session *session, const led_project_asset_id *id, int force);

/** Import queue state (synchronous execution in Phase 32;
 *  worker-ready shape for the future). */
typedef struct led_import_queue_stats {
    uint32_t pending;
    uint32_t running;
    uint32_t completed;
    uint32_t failed;
} led_import_queue_stats;

LED_API void led_import_queue_get_stats(
    const led_session *session, led_import_queue_stats *out_stats);

/* ---- asset browser model (headless; selection by UUID) ---- */

/** One folder-tree node (borrowed path storage, session-owned). */
typedef struct led_browser_folder {
    char path[1024]; /* project-relative; "" = root */
    uint32_t asset_count; /* assets directly inside */
    uint32_t folder_count; /* subfolders directly inside */
} led_browser_folder;

/** Refresh the browser model (rebuild folder tree + sorted asset
 *  list; 0 when no project). Returns asset count. */
LED_API uint32_t led_browser_refresh(led_session *session);

/** Folder / asset counts + borrowing (valid until next refresh /
 *  scan / close / detach / destroy). */
LED_API uint32_t led_browser_folder_count(
    const led_session *session);
LED_API const led_browser_folder *led_browser_folders(
    const led_session *session);
LED_API uint32_t led_browser_asset_count(
    const led_session *session);
LED_API int led_browser_asset_at(const led_session *session,
                                 uint32_t index,
                                 led_asset_record *out_record);

/** Browser filter + search (sessions-owned view; refresh rebuilds
 *  then applies). Type filter COUNT = all. Search is the same
 *  case-insensitive substring as the DB. Sort modes: 0 = path,
 *  1 = name, 2 = type-then-path (deterministic). */
LED_API led_result led_browser_set_filter(
    led_session *session, led_project_asset_type type,
    const char *search, int sort_mode);

/** Browser selection by project UUID (ordered unique, cap 256). */
LED_API led_result led_browser_select(led_session *session,
                                      const led_project_asset_id *id);
LED_API led_result led_browser_deselect(
    led_session *session, const led_project_asset_id *id);
LED_API led_result led_browser_clear_selection(
    led_session *session);
LED_API uint32_t led_browser_get_selection(
    const led_session *session, led_project_asset_id *out_ids,
    uint32_t capacity);

/** Headless asset inspector: metadata rows for one record (ID,
 *  type, path, status, importer, fingerprint, deps, diagnostics).
 *  Reuses led_inspector_row shape with LED_DATA_* types. Returns
 *  row count (rows session-owned). */
LED_API uint32_t led_browser_inspect(
    led_session *session, const led_project_asset_id *id);

/* ---- drag / drop (GUI-independent payloads, never raw paths) ---- */

/** Drag payload (project UUID + type; resolved via the DB). */
typedef struct led_drag_payload {
    led_project_asset_id asset;
    led_project_asset_type type;
} led_drag_payload;

/** Begin a drag for the current single browser selection (1/0). */
LED_API int led_drag_begin(led_session *session,
                           led_drag_payload *out_payload);

/** Drop a model/mesh asset into the scene: CREATE + assign via
 *  normal editor commands (undoable). Returns 1 on success. */
LED_API int led_drop_model_into_scene(
    led_session *session, const led_drag_payload *payload,
    const float position[3]);

/** Drop a material onto a renderable object: typed assign through
 *  a normal command (rejects type mismatches). Returns 1. */
LED_API int led_drop_material_onto_object(
    led_session *session, const led_drag_payload *payload,
    const le_object *target);

/** Drop a script onto an object: add/configure Script component
 *  through a normal command. Returns 1. */
LED_API int led_drop_script_onto_object(
    led_session *session, const led_drag_payload *payload,
    const le_object *target);

/** Open a scene payload (maps to led_scene_open; never accidental
 *  instantiate). Returns led_result. */
LED_API led_result led_open_scene_payload(
    led_session *session, const led_drag_payload *payload);

/* ---- prefabs (reusable subtree assets, .luprefab) ---- */

/** Prefab file format version (1 in Phase 32; load rejects
 *  anything else with LED_ERROR_PARSE). */
#define LED_PREFAB_FORMAT_VERSION ((uint32_t)1)

/** Create a prefab from a subtree (selected object + descendants):
 *  mints prefab-local UUIDs, captures authoring state (no runtime
 *  state), writes the .luprefab file + sidecar + DB record
 *  transactionally (failed write leaves no partial asset).
 *  Nested-prefab references inside the subtree are rejected. */
LED_API led_result led_prefab_create(led_session *session,
                                     const le_object *root,
                                     const char *rel_path);

/** Load + validate a prefab file into a registry prefab asset
 *  (transactional parse; malformed -> PARSE, world untouched).
 *  Returns the registry handle when out != NULL. */
LED_API led_result led_prefab_load(led_session *session,
                                   const char *rel_path,
                                   le_asset *out_asset);

/** Instantiate a prefab into the edit world (fresh scene IDs +
 *  engine commit path; returns the instance root + local->runtime
 *  map for editor tracking). Direct API (the undoable path is
 *  LED_CMD_INSTANTIATE_PREFAB via led_execute). */
typedef struct led_prefab_instance {
    le_object root;
    le_scene_object_id *local_ids;
    le_object *objects;
    uint32_t count;
    le_asset prefab_asset;
    led_project_asset_id project_id;
} led_prefab_instance;

LED_API led_result led_prefab_instantiate(
    led_session *session, const le_asset *prefab_asset,
    led_prefab_instance *out_instance);

/** Release an instance record (mapping arrays only; world objects
 *  stay live). NULL-safe no-op. */
LED_API void led_prefab_instance_free(
    led_prefab_instance *instance);

/* ------------------------------------------------------------------
 * Interactive graphical editor (Phase 33): immediate-mode desktop
 * view over the headless EditorCore above. C ABI over opaque
 * handles; no C++ types and no vendored-GUI headers cross this
 * boundary (GUI confinement).
 *
 * Layering: GUI (luma_editor_gui, C++) -> EditorCore (led_*) ->
 * engine -> assets -> renderer -> LumaC. The GUI talks to the
 * session ONLY through led_* (commands/history/inspect/viewport/
 * play/project/browser/prefab) and to the GPU ONLY through public
 * lc_* (window/input events in, scene+GUI draws out). No
 * engine/renderer/LumaC header names this layer back.
 *
 * Ownership: the host owns lc_window/lc_device/lc_swapchain,
 * le_engine/le_world, and led_session lifetimes. leg_* borrows them
 * (never destroys). At most one GUI context per led_session.
 * Threading: single-threaded on the world's owning thread (same
 * contract as the editor and engine).
 * ------------------------------------------------------------------ */

/** Opaque GUI context (immediate-mode context + font atlas + draw
 *  bridge + viewport texture state). Never dereference. */
typedef struct leg_context leg_context;

/** GUI frame input (plain values; host fills from its window/event
 *  pump each frame before leg_frame_begin). */
typedef struct leg_frame_input {
    uint32_t window_width;  /* client px (lc_window_get_width) */
    uint32_t window_height; /* client px (lc_window_get_height) */
    float delta_seconds;    /* clamped by the GUI to (0, 0.25] */
    int window_focused;     /* nonzero when the window has focus */
} leg_frame_input;

/** Create a GUI context bound to one session + device (both
 *  borrowed, must outlive the context). Session must be attached;
 *  device must be live. Enables upstream docking
 *  (io.ConfigFlags |= DockingEnable) and separates the layout INI
 *  (see leg_set_ini_path; never the scene/prefab path).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (NULL args,
 *          detached session, dead device), LED_ERROR_OUT_OF_MEMORY,
 *          LED_ERROR_UNAVAILABLE (C++ runtime refusal). */
LED_API led_result leg_context_create(led_session *session,
                                      lc_device *device,
                                      leg_context **out_context);

/** Destroy a GUI context (frees font/GPU bridge objects owned here;
 *  borrowed session/device/window handles untouched). NULL-safe. */
LED_API void leg_context_destroy(leg_context *context);

/** Override the layout INI path (copied; default "luma_editor.ini"
 *  under the process CWD discipline — never beside a scene/prefab).
 *  NULL/empty restores the default. */
LED_API led_result leg_set_ini_path(leg_context *context,
                                    const char *path);

/** Begin a GUI frame: feeds queued lc_window_event values drained
 *  from `window` (lc_window_read_event) into the context, uploads
 *  pending font/texture updates, and opens the dockspace root.
 *  `input` carries the window extent + dt + focus. Zero window
 *  extent = minimized: frame state stays valid, drawing is skipped
 *  until leg_frame_end (caller should skip present, keep pumping).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (NULL args),
 *          LED_ERROR_UNAVAILABLE (no context/GUI refusal). */
LED_API led_result leg_frame_begin(leg_context *context,
                                   lc_window *window,
                                   const leg_frame_input *input);

/** End a GUI frame: closes the dockspace root and renders the
 *  draw data (validates it; user callbacks never execute — they
 *  are skipped, counted, and reported). Drawing is skipped when the
 *  frame began minimized. */
LED_API led_result leg_frame_end(leg_context *context);

/** Record all editor panels for the current frame (menu, toolbar,
 *  viewport, hierarchy, inspector, assets, console, status). Call
 *  between leg_frame_begin and leg_frame_end; `viewport` is the
 *  host-owned orbit state the viewport panel + gizmo overlay drive
 *  (may be NULL for a headless frame — panels render, camera and
 *  picking idle). `dt` is the frame delta in seconds (clamped by
 *  the panels to (0, 0.25]). NULL context is a no-op (NULL-safe so
 *  hosts can call unconditionally). */
LED_API void leg_panels_frame(leg_context *context,
                              led_viewport *viewport, float dt);

/** Record the current frame's GUI draws into the caller's open pass
 *  (Phase 33 draw walk: blended pipeline, per-draw scissor, font
 *  texture sync). Call between leg_frame_end and present, inside ONE
 *  open lc_* pass whose color target uses `target_format` AND whose
 *  depth attachment (if any) uses `depth_format` (LC_FORMAT_UNDEFINED
 *  for depthless passes). The GUI pipeline is depthless-aware: it is
 *  created for the (color, depth-or-UNDEFINED) pair and rebuilds on
 *  drift (swapchain recreations keep the pair flowing from
 *  lc_swapchain_get_format/depth_format). Empty frames record
 *  nothing and succeed. User callbacks never execute (skipped,
 *  counted in stats).
 *  @return LED_SUCCESS, LED_ERROR_INVALID_ARGUMENT (NULL args, no
 *          frame ended this cycle, dead device, bad color format),
 *          LED_ERROR_UNAVAILABLE (texture/buffer/pipeline failure —
 *          frame stays alive, skip present or present without GUI),
 *          LED_ERROR_OVERFLOW (draw budget exceeded — same recovery).
 */
LED_API led_result leg_record_gui(leg_context *context,
                                  lc_command_encoder *encoder,
                                  lc_format target_format,
                                  lc_format depth_format);

/** Draw-walk budget report (plain data; zeros for NULL). */
typedef struct leg_draw_stats {
    uint32_t cmd_lists;
    uint32_t draw_cmds;
    uint32_t user_callbacks_skipped;
    uint32_t vertices;
    uint32_t indices;
    uint64_t bytes_estimate;
} leg_draw_stats;

LED_API void leg_draw_get_stats(const leg_context *context,
                                leg_draw_stats *out_stats);

/** Feed ONE window event into the context (pure input mapping;
 *  headless-testable without a frame in flight). Returns 1 when the
 *  event was consumed as GUI input, 0 when ignored (NULL-safe).
 *  Focus/close/resize bookkeeping stays host-owned (the host reads
 *  the same queue for its own loop); this call never drains. */
LED_API int leg_feed_event(leg_context *context,
                           const lc_window_event *event);

/** Input-ownership query AFTER leg_frame_begin (before rendering):
 *  nonzero when the GUI wants keyboard/mouse (panels/fields
 *  hovered/focused) and the viewport camera should yield. Pure
 *  function of context state; 0 for NULL. */
LED_API int leg_wants_keyboard(const leg_context *context);
LED_API int leg_wants_mouse(const leg_context *context);

/** Clamp a GUI clip rect (draw-command clip rect, DisplayPos-relative
 *  float px) into an lc_scissor_rect for the pass extent (pure math,
 *  headless-testable): floors mins, ceils maxes, intersects with
 *  [0, pass_w] x [0, pass_h]. Returns 1 + fill when the intersection
 *  is nonempty, 0 with zeroed out (empty/clipped-away) otherwise.
 *  NULL out still reports membership (counting-probe discipline, like
 *  led_assetdb_search): 1 when the rect intersects, 0 when empty.
 *  NaN/Inf inputs clamp to empty (0). NULL-safe pass extents (0). */
LED_API int leg_clip_to_scissor(float clip_min_x, float clip_min_y,
                                float clip_max_x, float clip_max_y,
                                uint32_t pass_w, uint32_t pass_h,
                                lc_scissor_rect *out_rect);

/** Last GUI record failure step (diagnostics; 0 = none/last-OK,
 *  1 gpu-ensure, 2 font-sync, 3 budget, 4 vtx buffer, 5 idx buffer,
 *  6 vtx upload, 7 idx upload, 8 bind pipeline, 9 bind vtx, 10 bind
 *  idx, 11 push, 12 sampler set, 13 per-draw tex resolve,
 *  14 bad input, 15 scissor, 16 bind tex set (OBSOLETE: merged into
 *  18), 17 draw-indexed, 18 bind-tex-set signature/live mismatch).
 *  Pure query; 0 for NULL. */
LED_API int leg_record_step_last(void);

/** Failing TexID for the last 13/18 record failure (0 when none).
 *  Pure query. */
LED_API unsigned long long leg_record_tex_last(void);

/** Opaque viewport target (panel-sized scene composite; owned by
 *  the GUI context, sized to the viewport panel, zero-size-safe,
 *  swapchain-independent). Never dereference. */
typedef struct leg_viewport_target leg_viewport_target;

/** Borrow-or-create the context's viewport target (NULL on bad
 *  context; context-owned, destroyed with it). */
LED_API struct leg_viewport_target *leg_viewport_target_for(
    leg_context *context);

/** Composite the session world into the panel-sized target (engine
 *  trio; enc must have NO open pass) + return the panel TexID (0
 *  when nothing to show — the panel falls back to its state
 *  readout). Play shows the runtime world, edit the edit world. */
LED_API unsigned long long leg_viewport_composite(
    leg_context *context, struct leg_viewport_target *vt,
    lc_command_encoder *enc, unsigned w, unsigned h);

/** Draw-walk budget probe (pure math, headless-testable): estimates
 *  vertex/index buffer bytes for `vertex_count` draw verts (pos+uv+
 *  col = 20 bytes) + `index_count` 16-bit indices. Always reports
 *  the full count in out bytes; out pointers may be NULL (counting
 *  query). Returns the vertex byte estimate. Caps at 64 MiB per
 *  buffer (LED_ERROR_OVERFLOW past it... returns the capped value
 *  with out_overflow set). */
LED_API uint64_t leg_draw_budget(uint32_t vertex_count,
                                 uint32_t index_count,
                                 uint64_t *out_vertex_bytes,
                                 uint64_t *out_index_bytes,
                                 int *out_overflow);

#ifdef __cplusplus
}
#endif

#endif /* LUMA_EDITOR_H */
