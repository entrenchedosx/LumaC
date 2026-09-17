/*
 * Luma Engine internals (never public, never includes LumaC or
 * renderer internals — public luma_engine.h + luma_renderer.h +
 * lumac.h only).
 *
 * Storage model:
 * - Object slots: dense array (slots[]) indexed by le_object.index.
 *   Each slot carries alive flag, generation, free-list link, name,
 *   enabled flag, presence bits, hierarchy links (parent index,
 *   first-child / next-sibling indices as INT32 with -1 = none),
 *   local transform, cached world matrix + dirty flag, and indices
 *   into the dense per-type component arrays (or -1).
 * - Free-list: singly linked stack of free slot indices (free_head,
 *   -1 = empty). Creation pops; destruction pushes. O(1), no scan.
 * - Components: dense arrays (renderables[], cameras[], lights[])
 *   with slot->entry back-links (entry_to_slot[]) so removal swaps
 *   the last entry down in O(1) while keeping the map consistent.
 * - World salt: per-world 64-bit random-ish salt (from address +
 *   counter mix at creation) folded into stable IDs so identical
 *   {index,generation} in two worlds never share a renderer key.
 * - Temporal key retirement: stable_id = mix(salt, index,
 *   generation). Destroying an object bumps its generation, so the
 *   key changes by construction. No separate retirement table is
 *   needed: renderer history reattaches only on exact key reuse.
 */

#ifndef LUMA_ENGINE_INTERNAL_H
#define LUMA_ENGINE_INTERNAL_H

#include <stdint.h>

#include "luma_engine/luma_engine.h"

#define LE_INVALID_SLOT ((uint32_t)0xFFFFFFFFu)
#define LE_NO_LINK ((int32_t)-1)
#define LE_INITIAL_CAPACITY 64u
#define LE_MAX_CAPACITY ((uint32_t)0x00FFFFFFu)

/* Presence bits (slot.present). */
#define LE_PRESENT_RENDERABLE ((uint32_t)(1u << 0))
#define LE_PRESENT_CAMERA ((uint32_t)(1u << 1))
#define LE_PRESENT_LIGHT ((uint32_t)(1u << 2))
/* Phase 25: asset-backed renderable (distinct from the Phase 24
 * pointer spelling; an object carries at most one renderable). */
#define LE_PRESENT_ASSET_RENDERABLE ((uint32_t)(1u << 3))
/* Phase 26: script component (at most one script per object). */
#define LE_PRESENT_SCRIPT ((uint32_t)(1u << 4))
/* Phase 28: physics components (rigid body + collider, at most
 * one of each per object). */
#define LE_PRESENT_RIGID_BODY ((uint32_t)(1u << 5))
#define LE_PRESENT_COLLIDER ((uint32_t)(1u << 6))
/* Phase 29: animator component (at most one per object). */
#define LE_PRESENT_ANIMATOR ((uint32_t)(1u << 7))
/* Phase 30: character controller (at most one per object). */
#define LE_PRESENT_CHARACTER ((uint32_t)(1u << 8))

typedef struct le_object_slot {
    uint32_t generation;
    int32_t free_next;
    int alive;
    int enabled;
    uint32_t present;
    int32_t parent;
    int32_t first_child;
    int32_t next_sibling;
    char *name;
    float position[3];
    float rotation[4];
    float scale[3];
    float world_matrix[16];
    int dirty;
    int32_t renderable_index;
    int32_t camera_index;
    int32_t light_index;
    /* Phase 25: persistent scene ID mapping for world<->scene
     * capture (nil until first capture assigns one). */
    le_scene_object_id scene_id;
    int has_scene_id;
    /* Phase 26: script component entry index (or LE_NO_LINK). */
    int32_t script_index;
    /* Phase 28: physics component entry indices (or LE_NO_LINK). */
    int32_t body_index;
    int32_t collider_index;
    /* Phase 29: animator component entry index (or LE_NO_LINK). */
    int32_t animator_index;
    /* Phase 30: character controller entry index (or LE_NO_LINK). */
    int32_t character_index;
} le_object_slot;

typedef struct le_renderable_entry {
    uint32_t slot;
    le_renderable_desc desc;
} le_renderable_entry;

/* Phase 25 asset-backed renderable entry (dense array mirroring
 * the pointer spelling; holds HANDLES, never renderer pointers). */
typedef struct le_asset_renderable_entry {
    uint32_t slot;
    le_asset_renderable_desc desc;
} le_asset_renderable_entry;

typedef struct le_camera_entry {
    uint32_t slot;
    le_camera_desc desc;
} le_camera_entry;

typedef struct le_light_entry {
    uint32_t slot;
    le_light_desc desc;
} le_light_entry;

/* Phase 26: script component entry (dense array, same swap-remove
 * discipline as every other component). `asset` is the runtime
 * script handle; lifecycle flags drive le_world_update dispatch;
 * `state_ref` is a backend-registry reference owned by the script
 * backend (-2 when no backend state exists yet — e.g. engine
 * without a script runtime, which never happens in practice since
 * add_script creates it, but teardown ordering must tolerate it). */
typedef struct le_script_entry {
    uint32_t slot;
    le_asset asset;
    int started;
    int failed;
    int pending_start;
    int state_ref;
} le_script_entry;

/* Phase 25: asset registry (engine-owned; le_asset handles index
 * it generationally). Slots carry type/state/persistent ID/source/
 * renderer backing/dependency links; the free-list mirrors object
 * storage (O(1) reuse, geometric growth, strong failure safety).
 * Renderables reference assets by HANDLE (never raw pointers), so
 * unload scans worlds for users — no back-pointer table to keep
 * consistent. Scene payloads ride LE_ASSET_SCENE slots as object-
 * record arrays. */
typedef struct le_asset_slot {
    int alive;
    uint32_t generation;
    int32_t free_next;
    le_asset_type type;
    le_asset_state state;
    le_asset_id id;
    char *source;
    /* Renderer backing (owned by the registry; exactly one kind
     * live per slot): mesh OR material OR texture view/image OR
     * scene records. */
    lr_mesh *mesh;
    lr_material *material;
    lc_image *texture_image;
    lc_image_view *texture_view;
    uint32_t texture_width;
    uint32_t texture_height;
    int texture_srgb;
    le_asset base_color_texture;
    int has_base_color_texture;
    le_asset metallic_roughness_texture;
    int has_metallic_roughness_texture;
    le_scene_object *scene_objects;
    uint32_t scene_count;
    uint32_t scene_capacity;
    /* Phase 26: script source (owned UTF-8 bytes) + compiled chunk
     * reference in the engine script registry (backend-owned int;
     * negative sentinel when uncompiled — always compiled for
     * READY scripts). */
    char *script_source;
    size_t script_size;
    int script_chunk_ref;
    /* Phase 29: animation assets (owned immutable data; exactly one
     * kind live per skeleton/clip slot). Skeleton = joint arrays;
     * clip = track arrays. See src/animation/animation_internal.h
     * for the layouts. */
    struct le_skeleton_data *skeleton;
    struct le_clip_data *clip;
    /* Phase 32: prefab payload (owned canonical text bytes + size;
     * exactly one kind live per prefab slot; no renderer backing). */
    char *prefab_text;
    size_t prefab_size;
} le_asset_slot;

struct le_engine {
    lr_renderer *renderer;
    le_world *worlds;
    /* Asset registry (Phase 25). */
    le_asset_slot *assets;
    uint32_t asset_capacity;
    uint32_t asset_alive;
    uint32_t asset_meshes;
    uint32_t asset_materials;
    uint32_t asset_textures;
    uint32_t asset_scenes;
    uint32_t asset_scripts;
    uint32_t asset_skeletons;
    uint32_t asset_clips;
    /* Phase 32: prefab census. */
    uint32_t asset_prefabs;
    uint32_t asset_ready;
    uint32_t asset_failed;
    int32_t asset_free_head;
    uint64_t asset_uuid_counter;
    uint64_t name_bytes_assets;
    /* glTF bridge manager (Phase 25 Stage 17): long-lived, owns
     * cached texture views borrowed by adopted materials. Created
     * lazily on first import; destroyed at engine shutdown AFTER
     * worlds + registry drain. Requires luma_assets linkage. */
    struct la_asset_manager *gltf_manager;
    /* Script runtime (Phase 26): one Lua backend per engine,
     * created lazily on first script use. Opaque to the rest of
     * the engine (defined in src/script/script_internal.h). */
    struct le_script_runtime *script_runtime;
    /* Input state (Phase 27): engine-owned, defined in
     * src/input/input_internal.h. Created with the engine. */
    struct le_input_state *input;
    /* Time + lifecycle state (Phase 27): engine-owned, defined in
     * src/time/time_internal.h. */
    struct le_time_state *time;
    int quit_requested;
    int has_focus;
    int minimized;
    lc_window *focus_window;
    le_cursor_mode cursor_mode;
    /* Phase 27: attached windows (borrowed; host-owned). The
     * engine drains their event queues each frame. */
    lc_window **windows;
    uint32_t window_count;
    uint32_t window_cap;
};

struct le_world {
    le_engine *engine;
    uint64_t salt;
    uint32_t tag;
    le_object_slot *slots;
    uint32_t capacity;
    uint32_t alive_count;
    uint32_t enabled_count;
    uint32_t named_count;
    uint32_t renderable_count;
    uint32_t camera_count;
    uint32_t light_count;
    int32_t free_head;
    le_renderable_entry *renderables;
    uint32_t renderable_capacity;
    le_asset_renderable_entry *asset_renderables;
    uint32_t asset_renderable_capacity;
    uint32_t asset_renderable_count;
    le_camera_entry *cameras;
    uint32_t camera_capacity;
    le_light_entry *lights;
    uint32_t light_capacity;
    le_script_entry *scripts;
    uint32_t script_capacity;
    uint32_t script_count;
    /* Phase 29: animator component entries (dense array, same
     * swap-remove discipline as every other component). Defined
     * in src/animation/animation_internal.h; general engine
     * sources touch animators only through the le_anim_* hooks
     * below. */
    struct le_animator_entry *animators;
    uint32_t animator_capacity;
    uint32_t animator_count;
    le_object active_camera;
    int has_active_camera;
    double time;
    le_render_report last_report;
    size_t name_bytes;
    le_world *next;
    le_world *prev;
    /* Phase 26: fixed-step script schedule + dispatch guards. */
    float script_fixed_dt;
    uint32_t script_max_steps;
    double script_accum;
    int scripts_firing;
    int scripts_tearing_down;
    /* Phase 27: per-world pause (independent of global time scale;
     * paused worlds skip simulation but still render). */
    int paused;
    /* Phase 28: per-world physics state (bodies, colliders, broad
     * phase, contacts, events). Created with the world. */
    struct le_physics_world *physics;
    /* Phase 30: character controller entries (dense array, same
     * swap-remove discipline as every other component). Defined
     * in src/physics/character.c; general engine sources touch
     * controllers only through the le_character_* API. */
    struct le_character_entry *characters;
    uint32_t character_capacity;
    uint32_t character_count;
};

/* ---- shared helpers (defined per-TU where used) ---- */

/* Resolve (world, object) to a live slot index. Returns 1 and sets
 * *out_slot on success; 0 with *out_result set to the precise
 * failure code on any bad input:
 * - NULL world/object -> LE_ERROR_INVALID_ARGUMENT;
 * - malformed handle (invalid encoding, generation 0, tag 0,
 *   index out of range) -> LE_ERROR_STALE_HANDLE (calling code
 *   that needs INVALID-vs-STALE distinction for NULL checks
 *   handles NULL before calling);
 * - tag mismatch (foreign world) -> LE_ERROR_WRONG_WORLD;
 * - dead slot or generation mismatch -> LE_ERROR_STALE_HANDLE. */
int le_resolve_live(const le_world *world, const le_object *object,
                    uint32_t *out_slot, le_result *out_result);

/* Mark a slot's subtree dirty (iterative, explicit stack; grows the
 * stack buffer geometrically, leaves the world unchanged on OOM —
 * returns 0 then). Returns 1 on success. */
int le_mark_subtree_dirty(le_world *world, uint32_t slot);

/* Refresh one slot's world matrix from its (already fresh)
 * ancestors: world = parent_world * local. Assumes callers order
 * root-first (le_refresh_world_matrices / le_get_world_matrix do). */
void le_compose_slot_world(le_world *world, uint32_t slot);

/* Refresh every dirty world matrix in the world (iterative
 * root-first order, dirty-driven ancestors-first; O(alive) worst
 * case, O(1) when clean). */
void le_refresh_world_matrices(le_world *world);

/* Engine frame simulation (sync.c): matrices + unconditional
 * script dispatch (dt==0 allowed for pause; NaN/Inf still
 * matrices-only). The engine lifecycle path uses this;
 * le_world_update keeps its legacy dt<=0-skips-scripts contract
 * for direct/test callers. */
void le_world_simulate_engine(le_world *world, float dt);

/* Ensure object capacity for one more live object (geometric
 * growth x2 with overflow + cap checks; preserves all storage on
 * failure). Returns LE_SUCCESS / OUT_OF_MEMORY / OVERFLOW. */
le_result le_ensure_object_capacity(le_world *world);

/* Detach a slot from its parent + sibling chain (no-op for roots).
 * Frees no memory; marks the detached subtree dirty. */
void le_detach_from_parent(le_world *world, uint32_t slot);

/* True when `ancestor` is an ancestor-or-self of `slot`
 * (iterative parent walk; used for cycle rejection). */
int le_is_ancestor(const le_world *world, uint32_t ancestor,
                   uint32_t slot);

/* ---- Phase 25 asset helpers (defined in asset.c) ---- */

/* Resolve (engine, asset) to a live slot index. Mirrors
 * le_resolve_live: NULL -> INVALID_ARGUMENT; malformed/dead/
 * generation mismatch -> STALE_ASSET. Returns 1 live, 0 dead. */
int le_resolve_asset_live(const le_engine *engine, const le_asset *asset,
                          uint32_t *out_slot, le_result *out_result);

/* Ensure registry capacity for one more live asset (geometric x2,
 * OOM-safe). Returns LE_SUCCESS / OUT_OF_MEMORY / OVERFLOW. */
le_result le_ensure_asset_capacity(le_engine *engine);

/* Allocate one asset slot of the given type/state/id/source
 * (source copied; NULL/"" allowed). Returns the slot or
 * LE_NO_LINK-class failure via out_result. */
int32_t le_asset_alloc(le_engine *engine, le_asset_type type,
                       le_asset_state state, const le_asset_id *id,
                       const char *source, le_result *out_result,
                       le_asset *out_handle);

/* Normalize a source path in place into `out` (out_size incl.
 * NUL): '/' separators, collapsed '.'/'..' (lexical, no
 * filesystem), no trailing slash (root kept). Returns 1 on
 * success, 0 when the path is NULL/empty/overlong/absolute-with-
 * drive weirdness (callers map to INVALID_ARGUMENT). Never reads
 * the filesystem; never follows symlinks. */
int le_normalize_source(const char *path, char *out, size_t out_size);

/* FNV-1a 64-bit over bytes (persistent content hashing). */
uint64_t le_fnv1a64(const void *bytes, size_t size);

/* Portable sub-asset identity (Phase 34A): derive the persistent
 * engine asset ID from a caller-supplied identity key (project
 * UUID bytes + stable sub-asset key string) instead of the file
 * access path. Location-free: identical keys yield identical IDs
 * on every machine and after any project relocation.
 *
 *   hi = FNV(key_bytes) ^ (FNV(sub_key) * P)
 *   lo = FNV(sub_key) ^ (hi | 1)      (nil-guarded to nonzero)
 *
 * NULL/empty keys fall back to the legacy path hash (documented
 * non-portable; kept for non-project callers). */
void le_identity_for_key(const void *key_bytes, size_t key_len,
                         const char *sub_key, le_asset_id *out_id);

/* Mint a fresh process-unique UUID into out (counter-seeded, never
 * nil; engine NULL mints from a static fallback — only used for
 * scene object IDs where the engine is always available). */
void le_uuid_mint(le_engine *engine, uint64_t *hi, uint64_t *lo);

/* Count live renderables (+ material texture pins) referencing an
 * asset slot across all worlds (for info + unload policy). */
uint32_t le_asset_refcount(const le_engine *engine, uint32_t slot);

/* ---- Phase 26 script hooks (defined in src/script/) ---- */

/* Opaque script runtime (defined in src/script/script_internal.h;
 * general engine sources touch it only through these hooks, so the
 * struct layout stays confined to the script backend). */
struct le_script_runtime;

/* Release one chunk token through the backend (engine.c/asset.c
 * teardown: they must not touch backend internals). No-op for NULL
 * runtime/chunk. */
void le_script_release_chunk_token(le_engine *engine, int chunk);

/* Destroy the engine script runtime (engine shutdown tail). */
void le_script_runtime_destroy(le_engine *engine);

/* Drive one world's script lifecycle for dt (called by
 * le_world_update for finite dt > 0): pending starts, fixed steps,
 * updates, in deterministic slot order over a snapshot. No-op for
 * NULL or scriptless worlds. */
void le_script_step_world(le_world *world, float dt);
/* Phase 28: fixed-step physics for scriptless worlds (same
 * accumulator/schedule as the PASS2 loop; called by
 * le_script_step_world when no script dispatch exists, never
 * alongside the inline loop). */
void le_script_step_physics(le_world *world, float dt);

/* Fire destroy() for every started script instance (world teardown
 * path; sets scripts_tearing_down so structural ops fail safely). */
void le_script_fire_world_destroy(le_world *world);

/* Fire destroy() for one slot's script iff started and not yet
 * fired (object-destroy / remove-script paths). Safe with no
 * script, unstarted, or already-fired instances. */
void le_script_fire_slot_destroy(le_world *world, uint32_t slot);
/* Phase 28: VM-independent collision/trigger dispatch into one
 * script entry (implemented in src/script/script_lua.c so the
 * physics TU never includes Lua; keeps general engine TUs free
 * of the script-internal header). Fires funcs[name](self, other,
 * contact); contact nil for EXIT. Returns 0 ok/absent, nonzero
 * on script error (caller marks failed). Valid names:
 * collision_enter/stay/exit, trigger_enter/stay/exit. */
int le_script_fire_collision(le_world *world,
                             le_script_entry *entry,
                             const char *name,
                             const le_object *other,
                             const float normal[3],
                             const float point[3],
                             float penetration);

/* Release backend state for one script entry (registry refs). */
void le_script_release_entry(le_world *world, le_script_entry *entry);

/* Tracked scene instances live here (script_step.c owns them). */
void le_script_free_world(le_world *world);

/* Capture/apply script records for scene.c without including
 * backend state. Serialize.c reads/writes script lines directly
 * from the public le_scene_object fields (no backend needed
 * there). le_scene_object is declared in luma_engine.h ABOVE
 * le_world (opaque here), so spell the struct tag-free typedef —
 * visible through engine_internal.h's luma_engine.h include. */
void le_script_capture_for_record(le_world *world, uint32_t slot,
                                  le_scene_object *rec);
le_result le_script_apply_record(le_world *world, const le_object *obj,
                                 const le_scene_object *rec);

/* ---- Phase 28 physics hook (defined in src/physics/) ---- */

struct le_physics_world;
struct le_physics_world *le_physics_create(void);
void le_physics_destroy(struct le_physics_world *pw);

/* Run one physics sub-step of dt (fixed-step ordering 2..8:
 * forces -> integrate -> detect -> solve -> sync -> events).
 * No-op for NULL world/physics or dt <= 0. Called once per
 * fixed interval from the PASS2 loop (script fixed_update
 * callbacks run first each interval — documented order). */
void le_physics_step(le_world *world, float dt);

/* Swap-remove a slot's body/collider entries (object destroy /
 * component strip path). Struct-blind hook so object.c never
 * touches le_physics_world layout. */
void le_physics_remove_slot_components(le_world *world,
                                       uint32_t slot);
/* Purge contacts/overlaps/events naming a retired slot (emits
 * EXIT to survivors). Defined in src/physics/physics.c. */
void le_physics_retire_slot(le_world *world, uint32_t slot);

/* Scene capture/apply for physics records (authoring state
 * only; mirrors the script record pattern). */
void le_physics_capture_for_record(le_world *world, uint32_t slot,
                                   le_scene_object *rec);
le_result le_physics_validate_record(const le_scene_object *rec);
le_result le_physics_apply_record(le_world *world,
                                  const le_object *obj,
                                  const le_scene_object *rec);

/* ---- Phase 30 character hooks (defined in src/physics/) ---- */

struct le_character_entry;

/* Swap-remove a slot's character entry (object destroy /
 * component strip path). Struct-blind hook so object.c never
 * touches character layout. */
void le_character_remove_slot(le_world *world, uint32_t slot);
/* Destroy all character state for a dying world. */
void le_character_destroy_world(le_world *world);

/* Scene capture/apply/validate for character records. */
void le_character_capture_for_record(le_world *world,
                                     uint32_t slot,
                                     le_scene_object *rec);
le_result le_character_validate_record(
    const le_scene_object *rec);
le_result le_character_apply_record(le_world *world,
                                    const le_object *obj,
                                    const le_scene_object *rec);

/* ---- Phase 29 animation hooks (defined in src/animation/) ---- */

struct le_skeleton_data;
struct le_clip_data;

/* Free one slot's animation asset backing (struct-blind for
 * asset.c/engine.c teardown). Either pointer may be NULL. */
void le_anim_free_slot_backing(struct le_skeleton_data *skeleton,
                               struct le_clip_data *clip);

/* Swap-remove a slot's animator entry (object destroy /
 * component strip path). Struct-blind hook so object.c never
 * touches animator layout. */
void le_anim_remove_slot_animator(le_world *world, uint32_t slot);

/* Destroy all animator runtime state for a dying world
 * (entries + pose/palette scratch; assets stay). */
void le_anim_destroy_world(le_world *world);

/* Advance animators + evaluate poses + write animated
 * transforms (variable-dt visual path; called from the PASS3
 * region of the dispatcher with scaled dt). No-op for NULL,
 * paused callers (pause handled by the caller), or dt<=0. */
void le_anim_step_visual(le_world *world, float dt);

/* Submit skin palettes for animated renderables (called from
 * the extraction path in sync.c, once per frame after the
 * visual step; dirty/version-gated uploads). */
void le_anim_submit_palettes(le_world *world);

/* Borrow one slot's evaluated skin palette for the submit /
 * extraction paths (0/NULL when no animator or no evaluated
 * pose; static poses evaluate on demand). Struct-blind for
 * sync.c. */
int le_anim_get_palette(le_world *world, uint32_t slot,
                        const float (**out_palette)[16],
                        uint32_t *out_joints);

/* Count animator references to one asset slot (struct-blind
 * for asset.c's refcount; skeleton + clip + fade dest). */
uint32_t le_anim_refcount_slot(const le_world *world,
                               uint32_t asset_slot,
                               uint32_t generation);

/* Scene capture/apply/validate for animator records. */
void le_anim_capture_for_record(le_world *world, uint32_t slot,
                                le_scene_object *rec);
le_result le_anim_validate_record(const le_scene_object *rec);
le_result le_anim_apply_record(le_world *world,
                               const le_object *obj,
                               const le_scene_object *rec);

#endif /* LUMA_ENGINE_INTERNAL_H */
