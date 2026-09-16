/*
 * Luma script runtime internals (Phase 26). Included ONLY by
 * src/script/*.c — never by general engine sources (which see Lua
 * through the three hooks in engine_internal.h) and never by public
 * headers (no lua_State * crosses luma_engine.h).
 *
 * Architecture reminder:
 *
 *                  Script Instance (le_object owner)
 *                              |
 *                    Script Backend API (this file)
 *                       /            \
 *            Lua backend (#1)     Future native backend
 *                  |                       |
 *               Lua VM               generated C
 *                  \                    /
 *                   Engine API (le_* only)
 *
 * The VM is an execution frontend. Gameplay semantics live in the
 * engine (handles, generations, world tags, persistent IDs,
 * deterministic order, transactional scenes).
 */

#ifndef LUMA_SCRIPT_INTERNAL_H
#define LUMA_SCRIPT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* LUA_NOREF lives in lua.h (== -2); engine sources that never
 * include Lua spell the sentinel literally. */
#define LE_SCRIPT_NOREF (-2)

/* Defaults (overridable via le_script_configure). */
#define LE_SCRIPT_DEFAULT_INSTRUCTIONS ((uint64_t)2000000)
#define LE_SCRIPT_HOOK_GRANULE 1024
#define LE_SCRIPT_DEFAULT_FIXED_DT 0.0f
#define LE_SCRIPT_DEFAULT_MAX_STEPS ((uint32_t)4)
#define LE_SCRIPT_MAX_SEARCH_PATHS 16u
#define LE_SCRIPT_MAX_SOURCE (4u * 1024u * 1024u)
#define LE_SCRIPT_MAX_ERROR 512u
#define LE_SCRIPT_MAX_TRACEBACK 2048u

/* VM-independent backend operations. Lua implements backend #1;
 * a future native backend implements the same table without Lua. */
typedef struct le_script_backend_ops {
    /* Compile source (validates syntax). Returns a backend chunk
     * token (>= 0) or < 0 with *out_error filled. */
    int (*compile)(struct le_script_runtime *rt, const char *source,
                   size_t size, const char *chunkname, char *out_error,
                   size_t error_cap);
    /* Release a chunk token. */
    void (*release_chunk)(struct le_script_runtime *rt, int chunk);
    /* Create instance state for an entry (runs the chunk with
     * export()/require sandbox; collects export metadata). Returns
     * 0 ok, nonzero with diagnostics recorded. */
    int (*instantiate)(struct le_script_runtime *rt, le_world *world,
                       le_script_entry *entry);
    /* Release instance state (state_ref etc.). */
    void (*release_instance)(struct le_script_runtime *rt,
                             le_world *world,
                             le_script_entry *entry);
    /* Fire one lifecycle callback (0=start,1=update,2=fixed,3=destroy).
     * dt is used for update/fixed. Returns 0 ok, nonzero = instance
     * failed (diagnostics recorded, caller disables). */
    int (*fire)(struct le_script_runtime *rt, le_world *world,
                le_script_entry *entry, int callback, float dt);
    /* Property access against live instance state. */
    int (*get_prop)(struct le_script_runtime *rt, le_world *world,
                    le_script_entry *entry, le_script_property *prop);
    int (*set_prop)(struct le_script_runtime *rt, le_world *world,
                    le_script_entry *entry,
                    const le_script_property *prop);
    /* List exported names (up to cap; full count in out_count). */
    int (*list_props)(struct le_script_runtime *rt, le_world *world,
                      le_script_entry *entry,
                      le_script_property *out, uint32_t cap,
                      uint32_t *out_count);
} le_script_backend_ops;

/* Engine-wide script runtime (one per le_engine). */
typedef struct le_script_runtime {
    const le_script_backend_ops *backend;
    lua_State *L;
    /* Config (le_script_config copy + search paths). */
    uint64_t max_instructions;
    uint64_t memory_budget;
    char script_root[1024];
    char search_paths[16][1024];
    uint32_t search_path_count;
    le_script_log_fn log_fn;
    void *log_user;
    /* Allocator accounting (Lua bytes now/peak). */
    size_t lua_bytes;
    size_t lua_peak;
    uint64_t alloc_count;
    int allocator_failed;
    /* Per-callback instruction budget state (hook scratch). */
    uint64_t instructions_left;
    int budget_exceeded;
    /* Diagnostics. */
    uint64_t callbacks_this_frame;
    uint32_t errors_total;
    char last_message[512];
    char last_traceback[2048];
    char last_source[256];
    char last_callback[32];
    le_object last_object;
    int has_last_object;
    int has_last_error;
    /* Firing world (set during dispatch so bindings resolve the
     * calling context without globals). */
    le_world *firing_world;
    int firing_depth;
} le_script_runtime;

/* Lua userdata layouts (shared by script_lua.c + bind TUs; the
 * scene spelling is layout-identical to the asset spelling — a
 * scene IS an asset-registry handle with a tag metatable). */
typedef struct le_lua_object {
    le_engine *engine;
    uint32_t world_tag;
    uint32_t index;
    uint32_t generation;
} le_lua_object;

typedef struct le_lua_asset {
    le_engine *engine;
    uint32_t index;
    uint32_t generation;
} le_lua_asset;

typedef le_lua_asset le_lua_scene;

#define LE_LUA_OBJECT_MT "le_object"
#define LE_LUA_ASSET_MT "le_asset"
#define LE_LUA_SCENE_MT "le_scene"
#define LE_LUA_INSTANCE_MT "le_scene_instance"

/* World diagnostics counter (engine aggregates per world state). */
uint32_t le_script_world_instance_count(const le_world *world,
                                        int *out_active,
                                        int *out_disabled,
                                        int *out_failed);

/* World-local tracked scene instances (implemented in
 * script_step.c; consumed by the World.instantiate binding). */
uint32_t le_script_track_instance(le_world *world,
                                  le_scene_instance *inst);
int le_script_instance_lookup_serial(le_world *world, uint32_t serial,
                                     const char *hex,
                                     le_object *out_obj);
uint32_t le_script_instance_count(le_world *world, uint32_t serial);
void le_script_free_world(le_world *world);

/* Rebind one live instance to its asset's current chunk (reload
 * path; implemented in script_step.c, backend work in
 * script_lua.c). */
void le_script_rebind_instance(le_world *world,
                               le_script_entry *entry);

/* --- backend (Lua) entry points, defined across script/*.c --- */

/* runtime */
le_result le_script_runtime_ensure(le_engine *engine);
void le_script_runtime_destroy(le_engine *engine);
void le_script_release_chunk_token(le_engine *engine, int chunk);
void le_script_record_error(le_script_runtime *rt, const char *callback,
                            le_world *world, const le_object *obj,
                            const char *source,
                            const char *message);
void le_script_log(le_script_runtime *rt, const char *message);

/* Lua backend ops table. */
extern const le_script_backend_ops le_lua_backend_ops;

/* bindings (each TU registers one surface; all share helpers). */
void le_lua_register_world(lua_State *L);
void le_lua_register_object(lua_State *L);
void le_lua_register_input(lua_State *L);
void le_lua_push_object(lua_State *L, le_world *world,
                        const le_object *obj);
int le_lua_check_object(lua_State *L, int idx, le_world **out_world,
                        le_object *out_obj);
void le_lua_push_asset(lua_State *L, le_engine *engine,
                       const le_asset *asset);
int le_lua_check_asset(lua_State *L, int idx, le_engine **out_engine,
                       le_asset *out_asset);
void le_lua_push_scene(lua_State *L, le_engine *engine,
                       const le_asset *scene);
int le_lua_check_scene(lua_State *L, int idx, le_engine **out_engine,
                       le_asset *out_scene);
/* Current firing context (NULL outside dispatch). */
le_script_runtime *le_lua_current_runtime(lua_State *L);

/* properties shared between export() and the C API. */
int le_script_prop_from_lua(lua_State *L, int idx, const char *name,
                            le_script_property *out);
void le_script_prop_to_lua(lua_State *L,
                           const le_script_property *prop);

/* Reload rebind (Lua side of le_script_rebind_instance). */
void le_lua_rebind(le_script_runtime *rt, le_world *world,
                   le_script_entry *entry);

/* Instruction-budget hook arms (script_runtime.c owns the hook). */
void le_script_hook_begin(le_script_runtime *rt);
void le_script_hook_end(le_script_runtime *rt);

#endif /* LUMA_SCRIPT_INTERNAL_H */
