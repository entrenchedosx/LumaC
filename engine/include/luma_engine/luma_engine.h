#ifndef LUMA_ENGINE_H
#define LUMA_ENGINE_H

/*
 * Luma Engine (Phase 24): generational scene objects, transforms,
 * hierarchy, and lightweight components on top of PUBLIC Luma
 * Renderer + LumaC only. Neither LumaC nor Luma Renderer depends on
 * this header.
 *
 * Layering (strict):
 *   Luma Engine may use Luma Renderer and LumaC. Neither may depend
 *   on Luma Engine. The renderer stays usable without this module.
 *   The engine never names backend symbols and never includes
 *   renderer-private or LumaC-private headers — only the public
 *   renderer and LumaC headers.
 *
 * Identity model (the core of this layer):
 * - Engine objects are generational handles (le_object =
 *   {index, generation}). Pointer identity, array position, renderer
 *   queue position, and GPU instance slots are NEVER durable
 *   identity. A stale handle (right index, wrong generation) fails
 *   every operation with LE_ERROR_STALE_HANDLE and never addresses
 *   a different live object.
 * - Renderer temporal identity is a stable 64-bit key derived from
 *   the engine handle plus a per-world salt (le_object_stable_id),
 *   submitted as lr_draw_item.instance_id. It drives renderer LOD
 *   hysteresis history, never queue position. Destroying an object
 *   retires its key: a slot-reusing successor never inherits the
 *   predecessor's temporal history.
 *
 * Conventions (project-wide, documented once):
 * - Column-major 4x4 matrices, meters, Y-up right-handed world space,
 *   camera looking along -Z in view space, Vulkan NDC (depth 0..1).
 *   Local matrices compose T * R * S; world(child) =
 *   world(parent) * local(child). Same convention as the renderer.
 * - Quaternions are (x, y, z, w) with w last. Same as the renderer.
 * - Ownership: the engine owns world storage (objects, transforms,
 *   hierarchy links, components, names); the application owns the
 *   engine, its worlds, and keeps lr_mesh / lr_material resources
 *   alive while renderables reference or submit them (renderer
 *   contract unchanged: destroys are NULL-safe, cross-renderer use
 *   is rejected, recording with dead objects fails safely).
 * - Destroy worlds before their renderer/device shuts down
 *   (dependents-first, like every Luma layer).
 * - Every fallible function returns le_result; NULL and stale
 *   handles fail with documented codes, never crash. All destroys
 *   are NULL-safe. Output pointers are cleared to NULL on failure
 *   where they are pointers; value getters zero-fill on NULL.
 * - Descriptors are zero-initializable; counts accompany pointer
 *   arrays; sizes are checked for overflow, never assumed away.
 *
 * Threading: world mutation is single-threaded (the owning thread).
 * Extraction (le_world_extract) runs on the same thread unless a
 * future phase documents otherwise. Read-only inspection is NOT
 * implicitly thread-safe. The engine spawns no threads, owns no
 * GPU synchronization (no barriers, fences, queues, semaphores —
 * those stay in LumaC/renderer), and allocates no GPU resources.
 */

#include <stddef.h>
#include <stdint.h>

#include <luma_renderer/luma_renderer.h>

/* Symbol visibility (mirrors LC_API/LR_API; the engine is currently
 * built STATIC so LE_API expands empty, but the macro keeps a future
 * shared build from silently dropping exports). */
#if defined(_WIN32) || defined(_WIN64)
    #if defined(LUMA_ENGINE_BUILD_SHARED)
        #if defined(LUMA_ENGINE_EXPORTS)
            #define LE_API __declspec(dllexport)
        #else
            #define LE_API __declspec(dllimport)
        #endif
    #else
        #define LE_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define LE_API __attribute__((visibility("default")))
    #else
        #define LE_API
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Engine result codes (lc_result/lr_result values never leak). Every
 * code is stable within its documented scope and safe to switch on
 * from C, future Lua bindings, editors, and MCP tooling. */
typedef enum le_result {
    LE_SUCCESS = 0,
    LE_ERROR_INVALID_ARGUMENT = 1,
    LE_ERROR_NOT_INITIALIZED = 2,
    LE_ERROR_OUT_OF_MEMORY = 3,
    LE_ERROR_STALE_HANDLE = 4,
    LE_ERROR_WRONG_WORLD = 5,
    LE_ERROR_MISSING_COMPONENT = 6,
    LE_ERROR_HAS_CHILDREN = 7,
    LE_ERROR_CYCLE = 8,
    LE_ERROR_OVERFLOW = 9,
    LE_ERROR_RENDERER = 10,
    LE_ERROR_UNSUPPORTED = 11,
    /* Phase 25: asset/scene/serialization errors (stable contract,
     * same switch-safety as the Phase 24 codes). */
    LE_ERROR_STALE_ASSET = 12,
    LE_ERROR_WRONG_ASSET_TYPE = 13,
    LE_ERROR_ASSET_IN_USE = 14,
    LE_ERROR_MISSING_ASSET = 15,
    LE_ERROR_PARSE = 16,
    LE_ERROR_UNSUPPORTED_VERSION = 17,
    LE_ERROR_DUPLICATE_ID = 18,
    LE_ERROR_INVALID_HIERARCHY = 19,
    LE_ERROR_UNREPRESENTABLE_TRANSFORM = 20,
    LE_ERROR_SERIALIZATION = 21
} le_result;

/* Opaque engine objects. Never dereference; use the API below. */
typedef struct le_engine le_engine;
typedef struct le_world le_world;

/* ------------------------------------------------------------------
 * Stable generational object handles.
 *
 * An le_object names a world slot (index) at a particular lifetime
 * (generation) in a particular world (world_tag). The world owns a
 * per-slot generation counter: creation returns {slot, current
 * generation, world tag}; destruction retires the slot and bumps
 * its generation (wrapping 0 -> 1, never 0, so 0 stays the
 * never-valid generation). A handle is live iff its world tag
 * matches the world the call names AND its slot is occupied AND
 * generations match. A handle from world A presented to world B
 * fails with LE_ERROR_WRONG_WORLD — even when B happens to hold a
 * live object at the same {index,generation} (which is likely:
 * both worlds allocate slot 0 first). Tags make cross-world
 * confusion structurally impossible instead of probabilistically
 * unlikely.
 *
 * The tag is assigned per world from a process-wide atomic counter
 * (never 0; 0 means "no world" and never validates). Tag reuse
 * after 4 billion worlds is documented: tags are process-local and
 * worlds validate liveness jointly with generations, so a tag
 * collision across world lifetimes still requires a simultaneous
 * {index,generation} collision on a live slot to alias — the same
 * bar as generation wrap itself.
 *
 * Generations are 32-bit: 4 billion reuses of one slot before an
 * ancient handle could alias a live object, and even then only if
 * the slot is occupied AND the tag matches (a destroyed-then-idle
 * slot never validates anything). Wrap policy is documented, not
 * silent: the counter skips 0 forever, so the invalid encoding is
 * unambiguous.
 *
 * le_object is an aggregate: brace-initialize handles positionally
 * ({index, generation, world_tag}) or compare against
 * LE_OBJECT_INVALID. Field order is stable ABI.
 * ------------------------------------------------------------------ */

typedef struct le_object {
    uint32_t index;
    uint32_t generation;
    uint32_t world_tag;
} le_object;

/* Explicit invalid object (index UINT32_MAX, generation 0, tag 0).
 * Never returned for a live object; every API rejects it. {0,0,0}
 * is NOT a valid object either (generation 0 and tag 0 never
 * validate). */
#define LE_OBJECT_INDEX_INVALID ((uint32_t)0xFFFFFFFFu)
extern const le_object LE_OBJECT_INVALID;

/** Nonzero when the handle could name a live object (not the invalid
 *  encoding, generation nonzero, tag nonzero). Does NOT touch any
 *  world: a well-formed handle may still be stale or foreign; use
 *  le_object_is_alive to validate against a world. NULL-safe
 *  (NULL -> 0). */
LE_API int le_object_is_valid(const le_object *object);

/** Nonzero when the handle names a LIVE object in this world (tag
 *  matches, slot occupied, generation matches). NULL world or NULL
 *  object -> 0. A tag mismatch returns 0 here (use any mutating API
 *  for the distinguishing LE_ERROR_WRONG_WORLD code). Never crashes
 *  on stale/cross-world input. */
LE_API int le_object_is_alive(const le_world *world,
                              const le_object *object);

/** Pack a live engine handle into the stable 64-bit renderer
 *  temporal key submitted as lr_draw_item.instance_id. The key mixes
 *  {world salt AND world tag, index, generation}: slot reuse changes
 *  the generation and therefore the key, so a successor never inherits
 *  a predecessor's LOD hysteresis history. Returns 0 for NULL
 *  world/object or non-live handles (0 = "no stable identity":
 *  the renderer runs that submission without hysteresis —
 *  deterministic, always safe). */
LE_API uint64_t le_object_stable_id(const le_world *world,
                                    const le_object *object);

/* ------------------------------------------------------------------
 * Engine (owns worlds + shared subsystem state; borrows nothing
 * except what descriptors name).
 * ------------------------------------------------------------------ */

/* Engine creation parameters. Zero-initialized is valid (all
 * defaults). `renderer` is optional: when non-NULL the engine may
 * validate renderable mesh/material liveness against it at sync
 * time; the renderer must outlive the engine. */
typedef struct le_engine_desc {
    lr_renderer *renderer;
} le_engine_desc;

/**
 * Create an engine.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL out),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_engine_create(const le_engine_desc *desc,
                                  le_engine **out_engine);

/** Destroy an engine and every world it still owns (worlds are
 *  engine-owned; destroying the engine retires all renderer
 *  temporal keys first). Safe with NULL. */
LE_API void le_engine_destroy(le_engine *engine);

/** Borrow the renderer this engine was created with (NULL when
 *  created without one, or for NULL engine). Borrowed: never
 *  destroy; it outlives the engine by contract. */
LE_API lr_renderer *le_engine_get_renderer(const le_engine *engine);

/* ------------------------------------------------------------------
 * World (owns engine objects + components; engine-owned itself).
 *
 * A world is the unit of object ownership, hierarchy, and render
 * extraction. Multiple worlds may coexist on one engine; handles
 * are world-local (cross-world use is LE_ERROR_WRONG_WORLD).
 * Streaming/multi-world scheduling is future work, but nothing in
 * this API assumes a single global world.
 * ------------------------------------------------------------------ */

/* World creation parameters. Zero-initialized is valid. When
 * nonzero, `initial_capacity` pre-sizes object storage (geometric
 * growth still applies beyond it); 0 selects the default. */
typedef struct le_world_desc {
    uint32_t initial_capacity;
} le_world_desc;

/**
 * Create a world owned by the engine.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine/out),
 *         LE_ERROR_OUT_OF_MEMORY (world unchanged on failure).
 */
LE_API le_result le_world_create(le_engine *engine,
                                 const le_world_desc *desc,
                                 le_world **out_world);

/**
 * Destroy a world and every live object/component it owns: hierarchy
 * links, transforms, renderables, cameras, lights, names, and
 * renderer temporal keys are all retired. Safe with NULL.
 * Renderer mesh/material resources are borrowed and untouched.
 */
LE_API void le_world_destroy(le_world *world);

/** Borrow the engine that owns this world (NULL for NULL). */
LE_API le_engine *le_world_get_engine(const le_world *world);

/* ------------------------------------------------------------------
 * Objects (creation, destruction, names, enabled state).
 *
 * Storage grows geometrically with overflow checks; allocation
 * failure leaves the world unchanged. Freed slots form a free-list
 * (O(1) reuse, no linear scan). Iteration order over live objects
 * is deterministic for unchanged world state (ascending slot
 * index).
 *
 * Destruction default: destroying a parent destroys its entire
 * subtree (depth-first, iteratively — no recursion, stack-safe at
 * any depth). Children never dangle; there is no "orphan to root"
 * mode in Phase 24.
 *
 * Structural mutation during iteration: the engine performs sync /
 * destroy-all passes over private snapshots; destroying objects
 * from application code while holding NO engine iterator is always
 * safe (there are no open iterators — queries return counts/copies
 * into caller memory). Callbacks do not exist yet, so no
 * callback-reentrancy hazard exists.
 * ------------------------------------------------------------------ */

/** Create one object (identity transform, enabled, no parent, no
 *  name, no optional components).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/out),
 *         LE_ERROR_OUT_OF_MEMORY / LE_ERROR_OVERFLOW (world
 *         unchanged on failure).
 */
LE_API le_result le_object_create(le_world *world, le_object *out_object);

/**
 * Destroy an object and its whole subtree: every descendant is
 * destroyed first (documented default), then the object itself is
 * detached, all its components are removed, its name freed, its
 * renderer temporal key retired, its slot generation bumped, and
 * the slot returned to the free-list. Stale handles to any
 * destroyed object fail every later operation. Safe with NULL
 * object (no-op success); destroying the invalid encoding is
 * LE_ERROR_INVALID_ARGUMENT; a stale handle is LE_ERROR_STALE_HANDLE.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_destroy(le_world *world,
                                   const le_object *object);

/** Live object count (0 for NULL). */
LE_API uint32_t le_world_get_object_count(const le_world *world);

/** Current object slot capacity (0 for NULL). Grows geometrically;
 *  always >= live count. */
LE_API uint32_t le_world_get_object_capacity(const le_world *world);

/* Object names: optional, caller-owned bytes copied in, owned by
 * the world. Identity NEVER depends on names; duplicates are
 * allowed (names serve debugging, future editor/serialization/Lua/
 * MCP). Lifetime: valid until the next set_name on the same object
 * or the object's destruction — never hold across those. */

/** Copy a name onto an object (NULL or "" clears it; the world
 *  keeps its own copy, so caller bytes may be transient).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_OUT_OF_MEMORY (old name kept on failure).
 */
LE_API le_result le_object_set_name(le_world *world,
                                    const le_object *object,
                                    const char *name);

/** Borrow an object's name ("" when unnamed; NULL for NULL world,
 *  NULL object, or stale/cross-world handles — never a dangling
 *  pointer). */
LE_API const char *le_object_get_name(const le_world *world,
                                      const le_object *object);

/* Enabled state: per-object flag. Effective visibility also folds
 * in ancestors (see le_object_is_effectively_enabled): a disabled
 * parent disables its whole subtree for submission. Renderable and
 * light sync consult the EFFECTIVE state; the stored flag is what
 * set/get report. */

/** Set the stored enabled flag (1 = enabled, 0 = disabled).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_set_enabled(le_world *world,
                                       const le_object *object,
                                       int enabled);

/** Copy out the stored enabled flag (1/0; 0 for NULL/stale input). */
LE_API int le_object_is_enabled(const le_world *world,
                                const le_object *object);

/** Effective enabled state: stored flag AND every ancestor's stored
 *  flag (root-to-object chain). 0 for NULL/stale input. A disabled
 *  parent hides its whole subtree from renderer submission and
 *  light submission. */
LE_API int le_object_is_effectively_enabled(const le_world *world,
                                            const le_object *object);

/* ------------------------------------------------------------------
 * Transforms (universal component: every object has exactly one).
 *
 * Canonical storage is position (vec3) + rotation (unit quaternion
 * x,y,z,w) + scale (vec3, may carry negative axes for mirroring).
 * New objects are identity (p=0, q=identity, s=1). Setters
 * normalize quaternions (zero-length input restores identity) and
 * reject non-finite components. Local matrices compose T * R * S
 * (column-major, renderer convention). World matrices compose
 * world(child) = world(parent) * local(child).
 *
 * Dirty propagation: any local change (position/rotation/scale/
 * reparent) marks the object and its whole descendant subtree
 * dirty. World matrices refresh lazily on read or eagerly at
 * le_world_update — never by recomputing the entire hierarchy per
 * setter. All hierarchy walks are iterative (explicit stacks), so
 * 100k-deep chains cannot overflow the call stack.
 * ------------------------------------------------------------------ */

/** Copy out position (zeros for NULL/stale input; out may be NULL
 *  for a no-op). */
LE_API void le_object_get_position(const le_world *world,
                                   const le_object *object,
                                   float out_position[3]);

/** Set position (all components must be finite).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object
 *         or non-finite), LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_set_position(le_world *world,
                                        const le_object *object,
                                        const float position[3]);

/** Copy out rotation quaternion (x,y,z,w; identity for NULL/stale
 *  input; out may be NULL for a no-op). Always unit length. */
LE_API void le_object_get_rotation(const le_world *world,
                                   const le_object *object,
                                   float out_rotation[4]);

/** Set rotation (need not be unit — normalized on store; a
 *  zero-length or non-finite input restores identity).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object
 *         or NULL input), LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_set_rotation(le_world *world,
                                        const le_object *object,
                                        const float rotation[4]);

/** Copy out scale ((1,1,1) for NULL/stale input; out may be NULL
 *  for a no-op). Negative axes are legal (mirroring). */
LE_API void le_object_get_scale(const le_world *world,
                                const le_object *object,
                                float out_scale[3]);

/** Set scale (all components must be finite; zero axes are
 *  allowed but produce singular matrices — world-matrix reads of
 *  a singular chain still succeed; normal-matrix consumers may
 *  reject them downstream).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object
 *         or non-finite), LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_set_scale(le_world *world,
                                     const le_object *object,
                                     const float scale[3]);

/** Compose the local T*R*S matrix (column-major 16 floats; identity
 *  for NULL/stale input; out may be NULL for a no-op). */
LE_API void le_object_get_local_matrix(const le_world *world,
                                       const le_object *object,
                                       float out_matrix[16]);

/** Copy out the cached world matrix, refreshing dirty ancestors
 *  first (column-major; identity for NULL/stale input; out may be
 *  NULL for a no-op). Iterative: safe at any hierarchy depth. */
LE_API void le_object_get_world_matrix(const le_world *world,
                                       const le_object *object,
                                       float out_matrix[16]);

/* ------------------------------------------------------------------
 * Hierarchy (parent/child links over stable handles).
 *
 * Reparenting preserves the LOCAL transform (documented Phase 24
 * policy): the child's local position/rotation/scale bytes are
 * untouched, so its world matrix changes with the new parent. A
 * world-preserving variant is future work. Cycles (self-parenting
 * or any ancestor loop) are rejected with LE_ERROR_CYCLE; the
 * world is unchanged on rejection.
 * ------------------------------------------------------------------ */

/** Attach child under parent (both live, same world). Either or
 *  both-NULL-as-LE_OBJECT_INVALID detaches: passing NULL (or a
 *  pointer to LE_OBJECT_INVALID) as parent makes child a root.
 *  Reparenting preserves local transform and dirties the child's
 *  subtree.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/child),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_CYCLE (child==parent or parent descends from
 *         child; world unchanged), LE_ERROR_OUT_OF_MEMORY (link
 *         allocation failure; world unchanged).
 */
LE_API le_result le_object_set_parent(le_world *world,
                                      const le_object *child,
                                      const le_object *parent);

/** Copy out a child's parent (root when out holds LE_OBJECT_INVALID
 *  afterwards). No-op for NULL world/child/out; stale handles yield
 *  LE_OBJECT_INVALID in out. Returns 1 when a parent exists, 0 for
 *  roots and bad input. */
LE_API int le_object_get_parent(const le_world *world,
                                const le_object *child,
                                le_object *out_parent);

/** Count live children (0 for NULL/stale input). */
LE_API uint32_t le_object_get_child_count(const le_world *world,
                                          const le_object *object);

/** List live children in deterministic order (ascending slot
 *  index): writes up to `capacity` handles into `out_children`,
 *  always reports the full count in `out_count` (either may be
 *  NULL). Stale/NULL input reports count 0.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object).
 */
LE_API le_result le_object_get_children(const le_world *world,
                                        const le_object *object,
                                        le_object *out_children,
                                        uint32_t capacity,
                                        uint32_t *out_count);

/** Count root objects (parentless live objects; 0 for NULL). */
LE_API uint32_t le_world_get_root_count(const le_world *world);

/* ------------------------------------------------------------------
 * Minimal component model (transform is universal; the rest below
 * are optional per-object records keyed by stable handle).
 *
 * Design: presence bits on the object slot + dense per-type arrays
 * with slot->entry index maps. Storage may relocate on growth, so
 * the API never exposes component addresses: access is always
 * (world, object) -> values. Removing an object removes all its
 * components. Component type IDs are small stable enums (never
 * pointer-derived, never hashed strings).
 * ------------------------------------------------------------------ */

/** Stable component type IDs (fixed contract; safe to persist in
 *  future scene files and switch on from bindings). */
typedef enum le_component_type {
    LE_COMPONENT_TRANSFORM = 0,
    LE_COMPONENT_RENDERABLE = 1,
    LE_COMPONENT_CAMERA = 2,
    LE_COMPONENT_LIGHT = 3,
    LE_COMPONENT_SCRIPT = 4,
    LE_COMPONENT_COUNT = 5
} le_component_type;

/** Nonzero when the object carries the component (transform is
 *  universal: always 1 for live objects). 0 for NULL/stale input
 *  or unknown types. */
LE_API int le_object_has_component(const le_world *world,
                                   const le_object *object,
                                   le_component_type type);

/* Renderable: references renderer-owned mesh/material (borrowed —
 * the application keeps them alive; cross-renderer or dead handles
 * fail the NEXT sync, never the add). Engine adds no new
 * material/mesh behavior: only fields the renderer honors. */

/** Renderable parameters (zero-init, then fill). `casts_shadow` /
 *  `receives_shadow` are opt-in (nonzero participates; zeroed stays
 *  inert, matching renderer convention). `visible` gates engine
 *  submission (invisible renderables never submit even when
 *  enabled). */
typedef struct le_renderable_desc {
    lr_mesh *mesh;
    lr_material *material;
    int casts_shadow;
    int receives_shadow;
    int visible;
} le_renderable_desc;

/** Attach (or replace) a renderable on a live object.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object/
 *         desc, NULL mesh/material), LE_ERROR_WRONG_WORLD,
 *         LE_ERROR_STALE_HANDLE, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_object_add_renderable(le_world *world,
                                          const le_object *object,
                                          const le_renderable_desc *desc);

/** Remove a renderable (missing component is success/no-op for
 *  idempotent teardown scripts).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_remove_renderable(le_world *world,
                                             const le_object *object);

/** Copy out renderable parameters (zeros for NULL/stale/missing;
 *  out may be NULL for a presence no-op). Returns 1 when present,
 *  0 otherwise. Mesh/material are the borrowed handles. */
LE_API int le_object_get_renderable(const le_world *world,
                                    const le_object *object,
                                    le_renderable_desc *out_desc);

/* Camera: engine-friendly lens owned by an object. The object's
 * world transform is the camera's transform (single authority):
 * sync derives an lr_camera (position + view + projection) from
 * the world matrix, honoring negative-determinant (mirrored)
 * ancestors by re-orthogonalizing the basis. */

/** Camera projection kind (only kinds the renderer honors today). */
typedef enum le_projection {
    LE_PROJECTION_PERSPECTIVE = 0,
    LE_PROJECTION_ORTHOGRAPHIC = 1
} le_projection;

/** Camera parameters (zero-init, then fill; `le_camera_desc_default`
 *  fills sane 60-degree perspective defaults). */
typedef struct le_camera_desc {
    le_projection projection;
    float fov_y_rad;
    float ortho_height;
    float aspect;
    float near_plane;
    float far_plane;
} le_camera_desc;

/** Fill defaults (perspective, 60 deg, aspect 16/9, near 0.1, far
 *  1000). NULL-safe no-op. */
LE_API void le_camera_desc_default(le_camera_desc *desc);

/** Attach (or replace) a camera on a live object. Rejects garbage
 *  lenses the same way the renderer does (non-positive FOV/aspect/
 *  near, far <= near, non-positive ortho height).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object/
 *         desc or bad lens), LE_ERROR_WRONG_WORLD,
 *         LE_ERROR_STALE_HANDLE, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_object_add_camera(le_world *world,
                                      const le_object *object,
                                      const le_camera_desc *desc);

/** Remove a camera (missing component is success/no-op).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_remove_camera(le_world *world,
                                         const le_object *object);

/** Copy out camera parameters (zeros for NULL/stale/missing; out
 *  may be NULL). Returns 1 when present, 0 otherwise. */
LE_API int le_object_get_camera(const le_world *world,
                                const le_object *object,
                                le_camera_desc *out_desc);

/** Designate (or clear) the world's active camera: the renderable
 *  camera that le_world_render uses. Pass NULL (or a pointer to
 *  LE_OBJECT_INVALID) to clear. The object must carry a camera.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_MISSING_COMPONENT (object has no camera).
 */
LE_API le_result le_world_set_active_camera(le_world *world,
                                            const le_object *object);

/** Copy out the active camera (LE_OBJECT_INVALID in out when none;
 *  no-op for NULL world/out). Returns 1 when a live camera is set
 *  (auto-cleared when its object dies), 0 otherwise. */
LE_API int le_world_get_active_camera(const le_world *world,
                                      le_object *out_object);

/* Light: minimal engine mirror of the renderer-supported kinds.
 * Position/direction derive from the object's world transform
 * (single authority, no duplicated transform). Spot direction is
 * the object's -Z axis transformed to world (travel direction,
 * renderer convention); point lights use world position. Disabled
 * (or effectively-disabled) light objects submit nothing. */

/** Engine light kinds (only kinds the renderer honors today). */
typedef enum le_light_type {
    LE_LIGHT_DIRECTIONAL = 0,
    LE_LIGHT_POINT = 1,
    LE_LIGHT_SPOT = 2
} le_light_type;

/** Light parameters (zero-init, then fill). Ranges/cones follow
 *  renderer validation (range > 0 for point/spot; 0 <= inner <=
 *  outer < pi/2 for spots; finite color/intensity). Shadow config
 *  rides through verbatim (point shadows are rejected at sync, as
 *  in the renderer). */
typedef struct le_light_desc {
    le_light_type type;
    float color[3];
    float intensity;
    float range;
    float spot_inner;
    float spot_outer;
    lr_shadow_desc shadow;
} le_light_desc;

/** Attach (or replace) a light on a live object.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object/
 *         desc, bad kind/range/cone/non-finite), LE_ERROR_WRONG_WORLD,
 *         LE_ERROR_STALE_HANDLE, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_object_add_light(le_world *world,
                                     const le_object *object,
                                     const le_light_desc *desc);

/** Remove a light (missing component is success/no-op).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_remove_light(le_world *world,
                                        const le_object *object);

/** Copy out light parameters (zeros for NULL/stale/missing; out
 *  may be NULL). Returns 1 when present, 0 otherwise. */
LE_API int le_object_get_light(const le_world *world,
                               const le_object *object,
                               le_light_desc *out_desc);

/* ------------------------------------------------------------------
 * World update + render extraction + renderer submission.
 *
 * Phase separation (deliberate, future-proof):
 *   simulation:  application mutates objects/components, then
 *                le_world_update(world, dt) refreshes world matrices
 *                (iterative, dirty-driven) and readies component state.
 *   extraction:  le_world_extract builds a flat render snapshot
 *                (world matrices + stable IDs + borrowed mesh/material
 *                + lights + active camera) into caller-visible counts
 *                or directly into the renderer queue. The snapshot is
 *                plain data: no renderer pointers leak into object
 *                identity, and a future multithreaded renderer can
 *                consume it off-thread.
 *   submission:  le_world_render performs extraction + renderer
 *                begin/submit/render_shadows/render_scene/render_output
 *                framing on the caller's open frame encoder.
 *
 * For Phase 24 extraction and submission share one call path; the
 * snapshot struct below is the documented boundary future phases
 * will widen (multithreading, editor, networking, fixed timestep).
 * ------------------------------------------------------------------ */

/** Refresh world matrices for every dirty subtree (iterative,
 *  dirty-driven; clean hierarchies cost O(1)). dt is accepted for
 *  the future simulation contract and currently only advances the
 *  world's time accumulator (negative/clamped to >= 0).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world).
 */
LE_API le_result le_world_update(le_world *world, float dt);

/** One extracted renderable (plain data for the render boundary:
 *  world matrix + stable temporal key + borrowed mesh/material +
 *  shadow/visibility flags + source object for debugging). */
typedef struct le_extracted_renderable {
    le_object object;
    float world_matrix[16];
    uint64_t stable_id;
    lr_mesh *mesh;
    lr_material *material;
    int casts_shadow;
    int receives_shadow;
    int mirrored;
} le_extracted_renderable;

/** One extracted light (renderer-ready lr_light + source object). */
typedef struct le_extracted_light {
    le_object object;
    lr_light light;
} le_extracted_light;

/** Extraction-docs snapshot sizes (filled by
 *  le_world_get_extraction_counts; zeros for NULL world). */
typedef struct le_extraction_counts {
    uint32_t renderables;
    uint32_t lights;
    int has_camera;
    lr_camera camera;
} le_extraction_counts;

/** Report how many renderables/lights the next extraction would
 *  submit and the active camera (zeros for NULL world; out may be
 *  NULL for a no-op). Counts respect enabled/effective-enabled,
 *  visibility flags, and component presence. Mesh/material liveness
 *  is NOT pre-filtered here (the renderer owns those registries and
 *  is the final authority at submit). */
LE_API void le_world_get_extraction_counts(const le_world *world,
                                           le_extraction_counts *out_counts);

/** Extract enabled renderables into caller memory (up to
 *  `capacity` entries, deterministic ascending-slot order; always
 *  reports the full count in `out_count`; either out pointer may be
 *  NULL for a counting query). Pure read: never touches the
 *  renderer. Returns LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *  world). */
LE_API le_result le_world_extract_renderables(
    le_world *world, le_extracted_renderable *out_items,
    uint32_t capacity, uint32_t *out_count);

/** Compute the lr_camera for this world's next submission (active
 *  camera's world matrix + lens, or the documented default
 *  60-degree origin camera when no live/effectively-enabled camera
 *  exists or its basis is singular). Pure read; out may be NULL
 *  for a no-op (returns 1 when an active camera feeds it, 0 for
 *  the fallback or NULL world). */
LE_API int le_world_get_render_camera(le_world *world, uint32_t width,
                                      uint32_t height,
                                      lr_camera *out_camera);

/** Submit this world's extracted frame to its engine renderer:
 *  le_world_update(0) + renderer begin (derived active camera) +
 *  light submits + renderable submits (world matrix + stable ID) +
 *  shadow prepare + render_scene into the renderer's HDR target.
 *  Scene pass only (no output/present): the caller composites or
 *  presents afterwards (le_world_render_output, swapchain pass, or
 *  custom passes) and owns the frame lifecycle: encoder must come
 *  from an open frame with NO open pass (dispatches run before
 *  passes open); the call leaves no pass open and the renderer
 *  frame OPEN (lr_renderer_end runs at le_world_render_end).
 *
 *  Split-scene/output exists so editors (thumbnails, material
 *  previews, multi-viewport) and tests (HDR readback, pixel
 *  proofs) can record scenes without presenting.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *         world/encoder, engine without renderer, zero extent),
 *         LE_ERROR_RENDERER (renderer begin/submit/prepare/render
 *         failure; the renderer frame stays open — call
 *         le_world_render_end to reset it), LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_world_render_scene(le_world *world,
                                       lc_command_encoder *encoder,
                                       uint32_t width, uint32_t height);

/** Tonemap the last le_world_render_scene HDR image into the
 *  caller's target (caller begins/ends the pass; any LDR target —
 *  swapchain or offscreen — works). Fails without a preceding
 *  render_scene since the last begin.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *         world/encoder/target, engine without renderer),
 *         LE_ERROR_RENDERER (no scene yet, or renderer failure).
 */
LE_API le_result le_world_render_output(le_world *world,
                                        lc_command_encoder *encoder,
                                        lc_render_target *target);

/** End the renderer frame opened by le_world_render_scene.
 *  NULL-safe no-op (also safe without an open scene). Copies the
 *  frame stats into the last-render report first. */
LE_API void le_world_render_end(le_world *world);

/** Legacy one-call frame: render_scene into the HDR target, then
 *  render_output into the caller's target inside an engine-opened
 *  pass (the scene pass opens/closes internally, so opening the
 *  output pass here is legal). For swapchain presentation prefer
 *  the explicit trio (render_scene; caller-begun swapchain pass +
 *  render_output; render_end) — a swapchain target passed here is
 *  rejected. The call leaves no pass open and the renderer frame
 *  CLOSED. Width/height fall back to the target extent when 0.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *         world/encoder/target, engine without renderer, swapchain
 *         target — pass rejection maps here),
 *         LE_ERROR_RENDERER (renderer failure), LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_world_render(le_world *world,
                                 lc_command_encoder *encoder,
                                 lc_render_target *target,
                                 uint32_t width, uint32_t height);

/** Last render submission accounting (zeros for NULL world; out
 *  may be NULL). submitted/visible mirror the renderer's frame
 *  stats for this world's items; skipped_disabled counts
 *  effectively-disabled renderables; skipped_invisible counts
 *  visible-flag-off renderables; skipped_dead counts borrowed
 *  mesh/material deaths observed at submit. */
typedef struct le_render_report {
    uint32_t submitted;
    uint32_t skipped_disabled;
    uint32_t skipped_invisible;
    uint32_t skipped_dead;
    lr_render_stats renderer_stats;
} le_render_report;

/** Copy out the last le_world_render accounting (zeros for NULL
 *  world; out may be NULL). */
LE_API void le_world_get_last_render_report(const le_world *world,
                                            le_render_report *out_report);

/* ------------------------------------------------------------------
 * Inspection + stats (structured, editor/MCP/Lua-ready; no native
 * handles, no reflection magic).
 * ------------------------------------------------------------------ */

/** Per-object debug snapshot (plain data; names borrowed — valid
 *  until the next set_name on the object or its destruction). */
typedef struct le_object_info {
    int alive;
    int enabled;
    int effectively_enabled;
    le_object parent;
    int has_parent;
    uint32_t child_count;
    int has_transform;
    int has_renderable;
    int has_camera;
    int has_light;
    const char *name;
} le_object_info;

/** Copy out one object's debug snapshot (zeros/"" for NULL/stale
 *  input; out may be NULL for a no-op). */
LE_API void le_object_get_info(const le_world *world,
                               const le_object *object,
                               le_object_info *out_info);

/** World statistics snapshot (plain counts; zeros for NULL). */
typedef struct le_world_stats {
    uint32_t objects_alive;
    uint32_t object_capacity;
    uint32_t root_count;
    uint32_t enabled_objects;
    uint32_t disabled_objects;
    uint32_t renderables;
    uint32_t cameras;
    uint32_t lights;
    uint32_t named_objects;
    double time;
} le_world_stats;

/** Copy out world statistics (zeros for NULL; out may be NULL). */
LE_API void le_world_get_stats(const le_world *world,
                               le_world_stats *out_stats);

/** Approximate engine CPU memory/capacity footprint (capacities x
 * -O2 struct sizes; ordinary malloc backing — no custom allocator
 *  claims). Zeros for NULL; out may be NULL. */
typedef struct le_memory_stats {
    uint64_t object_slots;
    uint64_t object_slot_bytes;
    uint64_t transform_bytes;
    uint64_t hierarchy_link_bytes;
    uint64_t renderable_bytes;
    uint64_t camera_bytes;
    uint64_t light_bytes;
    uint64_t name_bytes;
    uint64_t total_bytes;
} le_memory_stats;

/** Copy out memory accounting (see above). */
LE_API void le_world_get_memory_stats(const le_world *world,
                                      le_memory_stats *out_stats);

/* ------------------------------------------------------------------
 * Small math helpers (engine-local copies of the minimum needed:
 * identity/normalize/multiply/matrix-compose. The renderer keeps
 * its own copies; neither layer includes the other's internals.
 * Exported so tests and future bindings share one implementation.)
 * ------------------------------------------------------------------ */

/** Column-major identity. NULL-safe no-op. */
LE_API void le_mat4_identity(float out_matrix[16]);

/** Column-major out = a * b (may alias inputs via an internal
 *  temporary). NULL-safe no-op on any NULL. */
LE_API void le_mat4_multiply(float out[16], const float a[16],
                             const float b[16]);

/** Compose T*R*S from position/quaternion/scale (column-major).
 *  NULL-safe no-op on any NULL. */
LE_API void le_transform_compose(const float position[3],
                                 const float rotation[4],
                                 const float scale[3],
                                 float out_matrix[16]);

/** Normalize a quaternion in place semantics (out = n/|n|;
 *  zero-length or non-finite input yields identity). NULL-safe
 *  no-op on any NULL. */
LE_API void le_quat_normalize(const float in[4], float out[4]);

/** Hamilton product out = a * b (a applied after b). NULL-safe
 *  no-op on any NULL. */
LE_API void le_quat_multiply(const float a[4], const float b[4],
                             float out[4]);

/** Axis-angle to quaternion (non-unit or degenerate axis yields
 *  identity). NULL-safe no-op on any NULL. */
LE_API void le_quat_from_axis_angle(const float axis[3], float angle_rad,
                                    float out_quat[4]);

/** Upper-3x3 determinant sign: 1 when mirrored (negative
 *  determinant), else 0. NULL -> 0. */
LE_API int le_matrix_is_mirrored(const float m[16]);

/* ------------------------------------------------------------------
 * Reparent modes (Phase 25; Phase 24 always kept local).
 * ------------------------------------------------------------------ */

/** Reparent policy: keep the child's local transform (world matrix
 *  follows the new parent) or keep its world matrix (local
 *  recomputed by decomposition). Stable ABI. */
typedef enum le_reparent_mode {
    LE_REPARENT_KEEP_LOCAL = 0,
    LE_REPARENT_KEEP_WORLD = 1
} le_reparent_mode;

/** Attach child under parent with an explicit mode (see
 *  le_object_set_parent for the mode-less Phase 24 spelling, which
 *  keeps local). KEEP_WORLD recomputes local as
 *  inverse(new_parent_world) x old_world via TRS decomposition;
 *  shear-inducing reparents fail with
 *  LE_ERROR_UNREPRESENTABLE_TRANSFORM and leave the world unchanged.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/child,
 *         bad mode), LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_CYCLE (world unchanged),
 *         LE_ERROR_UNREPRESENTABLE_TRANSFORM (KEEP_WORLD only, world
 *         unchanged), LE_ERROR_OUT_OF_MEMORY (world unchanged).
 */
LE_API le_result le_object_reparent(le_world *world,
                                    const le_object *child,
                                    const le_object *parent,
                                    le_reparent_mode mode);

/* ------------------------------------------------------------------
 * TRS decomposition (Phase 25, for KEEP_WORLD reparenting).
 *
 * Decomposes a column-major matrix into translation + unit
 * quaternion + scale. Succeeds exactly for rotation+scale matrices
 * (including mirrors and non-uniform scales — the mirror sign lands
 * on the smallest-magnitude scale axis, matching submit-time
 * parity). Residual shear (from rotated non-uniform ancestors, or
 * hand-built matrices) has no exact TRS form: the call fails with
 * LE_ERROR_UNREPRESENTABLE_TRANSFORM rather than silently
 * distorting the transform.
 * ------------------------------------------------------------------ */

/** Decompose matrix into position/quaternion/scale (any out pointer
 *  may be NULL to skip that channel).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL matrix),
 *         LE_ERROR_UNREPRESENTABLE_TRANSFORM (shear beyond float
 *         tolerance, or a degenerate/singular basis axis).
 */
LE_API le_result le_matrix_decompose(const float matrix[16],
                                     float out_position[3],
                                     float out_rotation[4],
                                     float out_scale[3]);

/* ------------------------------------------------------------------
 * Engine assets (Phase 25): generational runtime handles over
 * engine-owned renderer resources + deterministic persistent IDs.
 *
 * Identity model (three distinct concepts — never conflated):
 * - le_asset: runtime handle {index, generation} into the ENGINE's
 *   registry. Temporary, like le_object. Stale handles fail safely.
 * - le_asset_id: persistent 128-bit identity (FNV-1a content hash
 *   for imported bytes, counter-seeded UUID for procedural assets).
 *   Stable across runs. What scene files store.
 * - source path: human locator (project-relative, normalized).
 *   NOT identity: two spellings of one file deduplicate to one
 *   asset; one path re-imported after unload gets a FRESH runtime
 *   handle but the SAME persistent ID (content hash).
 *
 * Domain: assets live on the ENGINE (le_engine), never in a world.
 * Every world on the engine may reference every READY asset.
 * Destroying a world never destroys assets. Engine shutdown drains
 * worlds first, then the registry (dependents-first, like every
 * Luma layer).
 *
 * Ownership: the registry OWNS the renderer resources
 * (lr_mesh / lr_material / views) and destroys them at unload via
 * the public renderer/LumaC APIs. Applications must NOT destroy a
 * renderer resource backing a live asset (the renderer registry
 * still guards liveness, so misuse fails safely instead of
 * corrupting the GPU).
 *
 * Lifetime: registry-owned until explicit unload (no GC). Unload
 * while renderables reference the asset fails with
 * LE_ERROR_ASSET_IN_USE (scan is O(objects); no per-asset
 * back-pointer table to keep consistent). Renderable components
 * hold runtime le_asset handles; extraction resolves them to
 * renderer pointers per frame, skipping non-READY assets with a
 * skipped_dead diagnostic (never a dangling pointer).
 * ------------------------------------------------------------------ */

/** Runtime asset handle (aggregate; brace-initialize or compare
 *  against LE_ASSET_INVALID). Field order is stable ABI. */
typedef struct le_asset {
    uint32_t index;
    uint32_t generation;
} le_asset;

/** Explicit invalid asset (index UINT32_MAX, generation 0). Never
 *  returned for a live asset; every API rejects it. */
#define LE_ASSET_INDEX_INVALID ((uint32_t)0xFFFFFFFFu)
extern const le_asset LE_ASSET_INVALID;

/** Asset kinds (only kinds Phase 25 populates; stable contract for
 *  scene files and bindings — never reordered). */
typedef enum le_asset_type {
    LE_ASSET_MESH = 0,
    LE_ASSET_MATERIAL = 1,
    LE_ASSET_TEXTURE = 2,
    LE_ASSET_SCENE = 3,
    LE_ASSET_SCRIPT = 4,
    LE_ASSET_COUNT = 5
} le_asset_type;

/** Asset load state (synchronous loads only in Phase 25; the model
 *  permits a future async loader — no fake async exists now). */
typedef enum le_asset_state {
    LE_ASSET_UNLOADED = 0,
    LE_ASSET_LOADING = 1,
    LE_ASSET_READY = 2,
    LE_ASSET_FAILED = 3
} le_asset_state;

/** Persistent 128-bit asset identity (two uint64s; stable across
 *  runs; printed/persisted as 32 lowercase hex digits). */
typedef struct le_asset_id {
    uint64_t hi;
    uint64_t lo;
} le_asset_id;

/** Nonzero when the handle could name a live asset (not the invalid
 *  encoding, generation nonzero). Does NOT touch the engine.
 *  NULL-safe (NULL -> 0). */
LE_API int le_asset_is_valid(const le_asset *asset);

/** Nonzero when the handle names a LIVE asset in this engine. NULL
 *  engine/asset -> 0. Never crashes on stale input. */
LE_API int le_asset_is_alive(const le_engine *engine,
                             const le_asset *asset);

/* Procedural mesh parameters (zero-init, then fill vertices/
 * indices; every index must address a live vertex; index_count a
 * nonzero multiple of 3). The registry copies CPU data only long
 * enough to upload, then owns the lr_mesh. */
typedef struct le_mesh_asset_desc {
    const lr_vertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
} le_mesh_asset_desc;

/** Create a READY mesh asset from CPU geometry (uploads through the
 *  public renderer API; malformed input creates nothing).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *         engine/out/desc, empty geometry, bad indices),
 *         LE_ERROR_OUT_OF_MEMORY, LE_ERROR_RENDERER (upload failure;
 *         nothing created).
 */
LE_API le_result le_asset_create_mesh(le_engine *engine,
                                      const le_mesh_asset_desc *desc,
                                      le_asset *out_asset);

/* Procedural PBR material parameters (zero-init; mirrors the subset
 * of lr_pbr_material_desc Phase 25 bridges: factors + optional
 * texture assets for base color and metallic-roughness). Texture
 * assets must be live READY LE_ASSET_TEXTURE handles on the same
 * engine; NULL selects the renderer fallback. Dependencies pin the
 * textures (unload-while-referenced is rejected). */
typedef struct le_material_asset_desc {
    float base_color_factor[4];
    float metallic_factor;
    float roughness_factor;
    le_asset base_color_texture;
    int has_base_color_texture;
    le_asset metallic_roughness_texture;
    int has_metallic_roughness_texture;
} le_material_asset_desc;

/** Create a READY material asset (texture deps must be live READY
 *  textures on this engine).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args, stale
 *         texture handles), LE_ERROR_WRONG_ASSET_TYPE (dependency
 *         is not a texture), LE_ERROR_OUT_OF_MEMORY,
 *         LE_ERROR_RENDERER.
 */
LE_API le_result le_asset_create_material(le_engine *engine,
                                          const le_material_asset_desc *desc,
                                          le_asset *out_asset);

/* Procedural texture parameters (zero-init; RGBA8 bytes, tightly
 * packed; srgb selects the sRGB GPU format). Uploads through public
 * LumaC APIs with a full mip chain. */
typedef struct le_texture_asset_desc {
    const unsigned char *rgba;
    uint32_t width;
    uint32_t height;
    int srgb;
} le_texture_asset_desc;

/** Create a READY texture asset from RGBA8 bytes.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args, empty
 *         image), LE_ERROR_OUT_OF_MEMORY, LE_ERROR_RENDERER.
 */
LE_API le_result le_asset_create_texture(le_engine *engine,
                                         const le_texture_asset_desc *desc,
                                         le_asset *out_asset);

/** Unload an asset and destroy its renderer backing (textures pinned
 *  by live materials, or any asset referenced by a live renderable
 *  in any world, fail with LE_ERROR_ASSET_IN_USE; world unchanged).
 *  Safe with NULL asset (no-op success); invalid encoding is
 *  LE_ERROR_INVALID_ARGUMENT; stale is LE_ERROR_STALE_ASSET.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_ASSET_IN_USE.
 */
LE_API le_result le_asset_unload(le_engine *engine,
                                 const le_asset *asset);

/** Copy out an asset's persistent ID (zeros for NULL/stale input;
 *  out may be NULL for a no-op). */
LE_API void le_asset_get_id(const le_engine *engine,
                            const le_asset *asset,
                            le_asset_id *out_id);

/** Asset type (LE_ASSET_COUNT for NULL/stale input). */
LE_API le_asset_type le_asset_get_type(const le_engine *engine,
                                       const le_asset *asset);

/** Load state (LE_ASSET_UNLOADED for NULL/stale input). READY
 *  assets resolve; nothing else submits. */
LE_API le_asset_state le_asset_get_state(const le_engine *engine,
                                         const le_asset *asset);

/** Borrow an asset's normalized source path ("" when procedural or
 *  NULL/stale — never a dangling pointer). */
LE_API const char *le_asset_get_source(const le_engine *engine,
                                       const le_asset *asset);

/* Renderable, Phase 25 form: mesh + material by ENGINE ASSET
 * handle (no persistent renderer pointers in the engine-facing
 * API). Zero-init, then fill; the Phase 24 lr_mesh/lr_material
 * spelling (le_object_add_renderable) keeps working for direct
 * renderer users and tests. */
typedef struct le_asset_renderable_desc {
    le_asset mesh;
    le_asset material;
    int casts_shadow;
    int receives_shadow;
    int visible;
} le_asset_renderable_desc;

/** Attach (or replace) an asset-backed renderable. Both assets must
 *  be live READY mesh/material handles on this world engine
 *  (validated now: stale/type/state failures return immediately,
 *  never at submit).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_STALE_ASSET, LE_ERROR_WRONG_ASSET_TYPE,
 *         LE_ERROR_MISSING_ASSET (not READY), LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_object_add_asset_renderable(
    le_world *world, const le_object *object,
    const le_asset_renderable_desc *desc);

/** Copy out asset-renderable parameters (zeros for NULL/stale/
 *  missing; out may be NULL). Returns 1 when an asset renderable
 *  is present, 0 otherwise (a Phase 24 pointer renderable reports
 *  0 here — the two spellings are distinct components). */
LE_API int le_object_get_asset_renderable(
    const le_world *world, const le_object *object,
    le_asset_renderable_desc *out_desc);

/** Remove an asset renderable (missing component is
 *  success/no-op).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_remove_asset_renderable(
    le_world *world, const le_object *object);

/* glTF bridge (Phase 25, reuses the existing importer — no new
 * parser): imports a .glb/.gltf file into mesh + material (+texture)
 * assets on this engine, deduplicated by canonical source path
 * (repeat loads return the SAME handles). Hierarchy rides the
 * returned node list (parent indices, local matrices) for scene
 * import; the caller owns the node array (free with
 * le_gltf_import_free). Partial failure destroys everything the
 * call created (transactional). */
typedef struct le_gltf_node {
    char name[128];
    int32_t parent;
    float local_matrix[16];
    int32_t mesh_asset;
    int32_t material_asset;
} le_gltf_node;

typedef struct le_gltf_import {
    le_asset *mesh_assets;
    uint32_t mesh_count;
    le_asset *material_assets;
    uint32_t material_count;
    le_asset *texture_assets;
    uint32_t texture_count;
    le_gltf_node *nodes;
    uint32_t node_count;
} le_gltf_result;

/** Import a glTF file into engine assets (dedup by canonical path).
 *  Needs an engine WITH a renderer (GPU upload). `out` receives
 *  owned arrays (free with le_gltf_import_free even on failure
 *  paths that partially filled it — on hard failure out is
 *  zeroed).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL
 *         engine/path/out, engine without renderer),
 *         LE_ERROR_MISSING_ASSET (file not found),
 *         LE_ERROR_PARSE (malformed — registry unchanged),
 *         LE_ERROR_RENDERER, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_gltf_import(le_engine *engine, const char *path,
                                le_gltf_result *out_import);

/** Release an import's handle arrays (handles stay live in the
 *  registry; only the listing is freed). NULL-safe no-op. */
LE_API void le_gltf_import_free(le_gltf_result *import);

/** Registry statistics snapshot (plain counts; zeros for NULL). */
typedef struct le_asset_stats {
    uint32_t assets_alive;
    uint32_t asset_capacity;
    uint32_t mesh_count;
    uint32_t material_count;
    uint32_t texture_count;
    uint32_t scene_count;
    uint32_t ready_count;
    uint32_t failed_count;
    uint64_t name_bytes;
} le_asset_stats;

/** Copy out registry stats (zeros for NULL; out may be NULL). */
LE_API void le_engine_get_asset_stats(const le_engine *engine,
                                      le_asset_stats *out_stats);

/** Structured asset inspection (plain data; source borrowed —
 *  valid until unload or engine shutdown). */
typedef struct le_asset_info {
    int alive;
    le_asset_type type;
    le_asset_state state;
    le_asset_id id;
    const char *source;
    uint32_t references;
} le_asset_info;

/** Copy out one asset's inspection snapshot (zeros/"" for
 *  NULL/stale input; out may be NULL). `references` counts live
 *  renderables (+ material texture pins) naming this asset. */
LE_API void le_asset_get_info(const le_engine *engine,
                              const le_asset *asset,
                              le_asset_info *out_info);

/** Borrow the renderer backing a READY mesh asset for tests/debug
 *  (NULL for NULL/stale/wrong-type/unready). Borrowed: never
 *  destroy; the registry owns it. */
LE_API lr_mesh *le_asset_get_mesh(const le_engine *engine,
                                  const le_asset *asset);

/** Borrow the renderer backing a READY material asset (same
 *  contract). */
LE_API lr_material *le_asset_get_material(const le_engine *engine,
                                          const le_asset *asset);

/* ------------------------------------------------------------------
 * Scenes (Phase 25): serialized project content vs runtime worlds.
 *
 * - le_scene is an ENGINE-owned asset payload (LE_ASSET_SCENE):
 *   object records with persistent IDs, hierarchy by persistent
 *   parent ID, canonical TRS, components, and asset references by
 *   persistent asset ID. Never runtime pointers, never runtime
 *   indices.
 * - A world is runtime state (le_object handles, cached matrices).
 *   world -> scene captures; scene -> world instantiates with FRESH
 *   runtime handles. The same scene instantiated twice yields
 *   disjoint handle sets (prefab-ready via the instance record).
 * - Renderer temporal identity stays unique per runtime object:
 *   stable keys mix the world salt + slot + generation, so two
 *   instances of one persistent ID never share LOD history.
 * ------------------------------------------------------------------ */

/** Persistent 128-bit scene-object identity (counter-seeded UUID;
 *  printed/persisted as 32 lowercase hex digits). Survives
 *  save/close/reload. Distinct from le_object (runtime) and from
 *  le_asset_id (asset content). */
typedef struct le_scene_object_id {
    uint64_t hi;
    uint64_t lo;
} le_scene_object_id;

/** Scene file format version (1 in Phase 25; loader rejects
 *  anything else with LE_ERROR_UNSUPPORTED_VERSION). */
#define LE_SCENE_FORMAT_VERSION ((uint32_t)1)

/* Exported script property (engine-defined metadata + value).
 * Declared here (above le_scene_object) because scene records
 * embed exported values. VM-independent by construction: metadata
 * is recorded by the export() declaration (name/type/default),
 * values convert losslessly between the backend and this struct.
 * A future native backend exposes identical metadata without
 * backend tables. */
typedef enum le_script_property_type {
    LE_SCRIPT_PROP_BOOL = 0,
    LE_SCRIPT_PROP_INT = 1,
    LE_SCRIPT_PROP_NUMBER = 2,
    LE_SCRIPT_PROP_STRING = 3,
    LE_SCRIPT_PROP_VEC3 = 4,
    LE_SCRIPT_PROP_ASSET = 5
} le_script_property_type;

typedef struct le_script_property {
    char name[64];
    le_script_property_type type;
    int boolean;
    int64_t integer;
    double number;
    char string_value[256];
    float vec3[3];
    le_asset asset;
} le_script_property;

/** One serializable scene object (plain data; asset references by
 *  persistent asset ID with a project-relative path hint for
 *  relocation; parent by persistent object ID). */
#define LE_SCRIPT_MAX_PROPS ((uint32_t)16)
typedef struct le_scene_object {
    le_scene_object_id id;
    le_scene_object_id parent;
    int has_parent;
    char name[128];
    int enabled;
    float position[3];
    float rotation[4];
    float scale[3];
    int has_renderable;
    le_asset_id mesh_id;
    le_asset_id material_id;
    int casts_shadow;
    int receives_shadow;
    int visible;
    int has_camera;
    le_camera_desc camera;
    int has_light;
    le_light_desc light;
    /* Phase 26: script component (asset ID + exported property
     * values; never VM state). */
    int has_script;
    le_asset_id script_id;
    le_script_property script_props[16];
    uint32_t script_prop_count;
} le_scene_object;

/** Create an empty scene payload owned by the engine (also
 *  registered as an LE_ASSET_SCENE asset when `register_asset` is
 *  nonzero, so scenes load by asset ID like everything else).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine/out),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_scene_create(le_engine *engine, int register_asset,
                                 le_asset *out_asset);

/** Append one object record (validates TRS finiteness, quaternion
 *  normalizability, and ID uniqueness; duplicates fail with
 *  LE_ERROR_DUPLICATE_ID, scene unchanged).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_DUPLICATE_ID,
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_scene_add_object(le_engine *engine,
                                     const le_asset *scene,
                                     const le_scene_object *object);

/** Object + root counts and format version (zeros for NULL/stale;
 *  out params may be NULL). Returns 1 for a live scene, 0
 *  otherwise. */
LE_API int le_scene_get_info(const le_engine *engine,
                             const le_asset *scene,
                             uint32_t *out_objects,
                             uint32_t *out_roots,
                             uint32_t *out_version);

/** Capture live world state into a scene asset (fresh persistent
 *  IDs for objects lacking scene-ID mapping; hierarchy, names,
 *  enabled, TRS, cameras, lights, and asset-backed renderables;
 *  pointer-backed Phase 24 renderables are SKIPPED with a count in
 *  out_skipped when non-NULL — scenes never store renderer
 *  pointers). Deterministic ascending-slot order.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/scene),
 *         LE_ERROR_WRONG_WORLD (scene on another engine),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_scene_capture(le_world *world, const le_asset *scene,
                                  uint32_t *out_skipped);

/** One instantiation of a scene into a world (prefab-ready record:
 *  fresh runtime handles + persistent->runtime mapping for Lua/
 *  editor/save-game use). */
typedef struct le_scene_instance {
    le_asset scene;
    le_object root;
    int has_root;
    le_scene_object_id *object_ids;
    le_object *objects;
    uint32_t count;
} le_scene_instance;

/** Instantiate a scene into a world (parse/validate/resolve FIRST;
 *  the world is untouched on failure — transactional. Forward
 *  parent refs, missing parents, self-parents, cycles, duplicate
 *  IDs, and unready/missing assets fail cleanly:
 *  LE_ERROR_INVALID_HIERARCHY / LE_ERROR_DUPLICATE_ID /
 *  LE_ERROR_MISSING_ASSET. Missing asset REFERENCES resolve at
 *  render as skips, but instantiation requires every referenced
 *  asset READY — a scene that names an unknown asset never
 *  half-instantiates).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_ASSET,
 *         LE_ERROR_DUPLICATE_ID, LE_ERROR_INVALID_HIERARCHY,
 *         LE_ERROR_MISSING_ASSET, LE_ERROR_OUT_OF_MEMORY,
 *         LE_ERROR_UNREPRESENTABLE_TRANSFORM (never for canonical
 *         TRS records; reserved for future extensions).
 */
LE_API le_result le_scene_instantiate(le_world *world,
                                      const le_asset *scene,
                                      le_scene_instance *out_instance);

/** Release an instance record (mapping arrays only; world objects
 *  stay live). NULL-safe no-op. */
LE_API void le_scene_instance_free(le_scene_instance *instance);

/** Look up the runtime handle for a persistent ID inside an
 *  instance (1 when found, 0 otherwise; out may be NULL for a
 *  presence query). */
LE_API int le_scene_instance_lookup(const le_scene_instance *instance,
                                    const le_scene_object_id *id,
                                    le_object *out_object);

/* Serialization: canonical text format (UTF-8, LF, versioned).
 * Memory-first (file helpers layer above): serialize to a malloc'd
 * buffer, parse from bytes. Byte-identical output for identical
 * scenes (ascending-ID order, fixed float formatting, sorted
 * keys). Unknown FIELDS are tolerated (forward compat); unknown
 * COMPONENTS and unknown VERSIONS are rejected. */

/** Serialize a scene asset to a malloc'd NUL-terminated buffer
 *  (caller frees with le_scene_free_text). Deterministic.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_SERIALIZATION,
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_scene_save_text(le_engine *engine,
                                    const le_asset *scene, char **out_text,
                                    size_t *out_size);

/** Release a buffer from le_scene_save_text (NULL-safe). */
LE_API void le_scene_free_text(char *text);

/** Parse + validate bytes into a scene asset WITHOUT touching any
 *  world (transactional: on failure the scene is empty/unchanged
 *  and the error maps precisely — truncation/EOF/bad version/
 *  overflow/count abuse/duplicates/cycles/NaN/bad enums all fail,
 *  never half-load).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_PARSE,
 *         LE_ERROR_UNSUPPORTED_VERSION, LE_ERROR_DUPLICATE_ID,
 *         LE_ERROR_INVALID_HIERARCHY, LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_scene_load_text(le_engine *engine,
                                    const le_asset *scene, const char *text,
                                    size_t size);

/** File helpers over the memory core (explicit paths; no CWD
 *  dependence beyond what the caller passes in).
 *
 * @return save: LE_SUCCESS / INVALID_ARGUMENT / STALE_ASSET /
 *         SERIALIZATION / OUT_OF_MEMORY, plus LE_ERROR_PARSE when
 *         the file cannot be written; load: as le_scene_load_text
 *         plus LE_ERROR_MISSING_ASSET when the file is absent.
 */
LE_API le_result le_scene_save_file(le_engine *engine,
                                    const le_asset *scene,
                                    const char *path);
LE_API le_result le_scene_load_file(le_engine *engine,
                                    const le_asset *scene,
                                    const char *path);

/** Persistent-ID helpers (all NULL-safe; zeros/0 for bad input).
 *  le_scene_object_id_make mints a fresh UUID (counter-seeded,
 *  process-unique); _is_nil tests the nil ID; _equal compares. */
LE_API void le_scene_object_id_make(le_engine *engine,
                                    le_scene_object_id *out_id);
LE_API int le_scene_object_id_is_nil(const le_scene_object_id *id);
LE_API int le_scene_object_id_equal(const le_scene_object_id *a,
                                    const le_scene_object_id *b);
LE_API void le_asset_id_to_string(const le_asset_id *id,
                                  char out_hex[33]);
LE_API int le_asset_id_from_string(const char *hex, le_asset_id *out_id);
LE_API int le_asset_id_equal(const le_asset_id *a,
                             const le_asset_id *b);
LE_API int le_asset_id_is_nil(const le_asset_id *id);

/* ------------------------------------------------------------------
 * Input, time, and frame lifecycle (Phase 27): engine-owned
 * gameplay runtime services. Lua consumes them through bindings;
 * future native/AOT scripts use these same C APIs directly.
 *
 * Input ownership: the ENGINE owns input state per le_engine.
 * Platform backends (Win32/X11) translate OS events into the
 * backend-neutral lc_window_event queue; the engine drains that
 * queue into one finalized per-frame snapshot shared by every
 * world on the engine. Worlds observe the same snapshot but keep
 * independent object/script state. No gameplay input logic lives
 * in Lua, the renderer, or any graphics backend.
 *
 * Threading: poll + advance + query on the owning thread only
 * (same contract as scripts). No thread safety is claimed.
 *
 * Coordinate convention: mouse position is client-area pixels,
 * origin top-left, +x right, +y down. Deltas are pixels of
 * relative motion accumulated since the previous input frame.
 * Wheel is detents (lines), up/right positive.
 * ------------------------------------------------------------------ */

/** Backend-neutral physical key identity (mirrors lc_keycode
 *  ordering; Luma-owned values, never Win32 VK_* or X11 KeySym). */
typedef enum le_key {
    LE_KEY_UNKNOWN = 0,
    LE_KEY_A = 1, LE_KEY_B, LE_KEY_C, LE_KEY_D, LE_KEY_E,
    LE_KEY_F, LE_KEY_G, LE_KEY_H, LE_KEY_I, LE_KEY_J, LE_KEY_K,
    LE_KEY_L, LE_KEY_M, LE_KEY_N, LE_KEY_O, LE_KEY_P, LE_KEY_Q,
    LE_KEY_R, LE_KEY_S, LE_KEY_T, LE_KEY_U, LE_KEY_V, LE_KEY_W,
    LE_KEY_X, LE_KEY_Y, LE_KEY_Z,
    LE_KEY_0 = 30, LE_KEY_1, LE_KEY_2, LE_KEY_3, LE_KEY_4,
    LE_KEY_5, LE_KEY_6, LE_KEY_7, LE_KEY_8, LE_KEY_9,
    LE_KEY_ESCAPE = 50, LE_KEY_ENTER, LE_KEY_TAB, LE_KEY_SPACE,
    LE_KEY_BACKSPACE,
    LE_KEY_LEFT_SHIFT = 60, LE_KEY_RIGHT_SHIFT,
    LE_KEY_LEFT_CONTROL, LE_KEY_RIGHT_CONTROL,
    LE_KEY_LEFT_ALT, LE_KEY_RIGHT_ALT,
    LE_KEY_LEFT_SUPER, LE_KEY_RIGHT_SUPER,
    LE_KEY_LEFT = 70, LE_KEY_RIGHT, LE_KEY_UP, LE_KEY_DOWN,
    LE_KEY_INSERT = 80, LE_KEY_DELETE, LE_KEY_HOME, LE_KEY_END,
    LE_KEY_PAGE_UP, LE_KEY_PAGE_DOWN,
    LE_KEY_F1 = 90, LE_KEY_F2, LE_KEY_F3, LE_KEY_F4, LE_KEY_F5,
    LE_KEY_F6, LE_KEY_F7, LE_KEY_F8, LE_KEY_F9, LE_KEY_F10,
    LE_KEY_F11, LE_KEY_F12,
    LE_KEY_NUMPAD_0 = 110, LE_KEY_NUMPAD_1, LE_KEY_NUMPAD_2,
    LE_KEY_NUMPAD_3, LE_KEY_NUMPAD_4, LE_KEY_NUMPAD_5,
    LE_KEY_NUMPAD_6, LE_KEY_NUMPAD_7, LE_KEY_NUMPAD_8,
    LE_KEY_NUMPAD_9, LE_KEY_NUMPAD_DECIMAL, LE_KEY_NUMPAD_DIVIDE,
    LE_KEY_NUMPAD_MULTIPLY, LE_KEY_NUMPAD_SUBTRACT,
    LE_KEY_NUMPAD_ADD, LE_KEY_NUMPAD_ENTER, LE_KEY_NUMPAD_EQUAL,
    LE_KEY_MINUS = 130, LE_KEY_EQUAL, LE_KEY_LEFT_BRACKET,
    LE_KEY_RIGHT_BRACKET, LE_KEY_BACKSLASH, LE_KEY_SEMICOLON,
    LE_KEY_APOSTROPHE, LE_KEY_GRAVE, LE_KEY_COMMA, LE_KEY_PERIOD,
    LE_KEY_SLASH, LE_KEY_CAPS_LOCK,
    LE_KEY_COUNT = 143
} le_key;

/** Backend-neutral mouse buttons. */
typedef enum le_mouse_button {
    LE_MOUSE_LEFT = 0,
    LE_MOUSE_RIGHT = 1,
    LE_MOUSE_MIDDLE = 2,
    LE_MOUSE_4 = 3,
    LE_MOUSE_5 = 4,
    LE_MOUSE_BUTTON_COUNT = 5
} le_mouse_button;

/** Backend-neutral gamepad buttons (engine API is real; platform
 *  reporting is PARTIAL in Phase 27 — see le_gamepad_is_connected). */
typedef enum le_gamepad_button {
    LE_GAMEPAD_A = 0, LE_GAMEPAD_B, LE_GAMEPAD_X, LE_GAMEPAD_Y,
    LE_GAMEPAD_LEFT_BUMPER, LE_GAMEPAD_RIGHT_BUMPER,
    LE_GAMEPAD_BACK, LE_GAMEPAD_START,
    LE_GAMEPAD_LEFT_STICK, LE_GAMEPAD_RIGHT_STICK,
    LE_GAMEPAD_DPAD_UP, LE_GAMEPAD_DPAD_DOWN,
    LE_GAMEPAD_DPAD_LEFT, LE_GAMEPAD_DPAD_RIGHT,
    LE_GAMEPAD_BUTTON_COUNT = 14
} le_gamepad_button;

/** Backend-neutral gamepad axes (sticks [-1,+1], triggers [0,1]). */
typedef enum le_gamepad_axis {
    LE_GAMEPAD_AXIS_LEFT_X = 0, LE_GAMEPAD_AXIS_LEFT_Y,
    LE_GAMEPAD_AXIS_RIGHT_X, LE_GAMEPAD_AXIS_RIGHT_Y,
    LE_GAMEPAD_AXIS_LEFT_TRIGGER, LE_GAMEPAD_AXIS_RIGHT_TRIGGER,
    LE_GAMEPAD_AXIS_COUNT = 6
} le_gamepad_axis;

/** Modifier snapshot (mirrors lc_key_mod). */
typedef enum le_key_mod {
    LE_MOD_NONE = 0,
    LE_MOD_SHIFT = 1 << 0,
    LE_MOD_CONTROL = 1 << 1,
    LE_MOD_ALT = 1 << 2,
    LE_MOD_SUPER = 1 << 3
} le_key_mod;

/** Cursor mode (request; the platform applies best-effort). */
typedef enum le_cursor_mode {
    LE_CURSOR_NORMAL = 0,
    LE_CURSOR_HIDDEN = 1,
    LE_CURSOR_CAPTURED = 2
} le_cursor_mode;

/** Opaque input action (index into the engine action registry;
 *  resolve once via le_input_find_action, then pass by value). */
typedef struct le_input_action {
    uint32_t index;
    uint32_t generation;
} le_input_action;

extern const le_input_action LE_INPUT_ACTION_INVALID;

/** Opaque input axis (same registry discipline as actions). */
typedef struct le_input_axis {
    uint32_t index;
    uint32_t generation;
} le_input_axis;

extern const le_input_axis LE_INPUT_AXIS_INVALID;

/** Opaque input context (a named map of actions/axes). */
typedef struct le_input_context {
    uint32_t index;
    uint32_t generation;
} le_input_context;

extern const le_input_context LE_INPUT_CONTEXT_INVALID;

/* Binding source kinds (one action/axis binding each). */
typedef enum le_binding_kind {
    LE_BINDING_KEY = 0,
    LE_BINDING_MOUSE_BUTTON = 1,
    LE_BINDING_GAMEPAD_BUTTON = 2,
    LE_BINDING_GAMEPAD_AXIS = 3, /* axis binding: full deflection */
    LE_BINDING_MOUSE_DELTA_X = 4,/* axis binding: pixels/frame */
    LE_BINDING_MOUSE_DELTA_Y = 5,
    LE_BINDING_MOUSE_WHEEL_X = 6,/* axis binding: detents/frame */
    LE_BINDING_MOUSE_WHEEL_Y = 7
} le_binding_kind;

typedef struct le_input_binding {
    le_binding_kind kind;
    /* Payload by kind: KEY -> key; MOUSE_BUTTON -> mouse_button;
     * GAMEPAD_BUTTON -> gamepad_button + gamepad_slot;
     * GAMEPAD_AXIS -> gamepad_axis + gamepad_slot. */
    le_key key;
    le_mouse_button mouse_button;
    le_gamepad_button gamepad_button;
    le_gamepad_axis gamepad_axis;
    uint32_t gamepad_slot;
    /* Axis bindings: which pole this binding drives (-1/+1) for
     * digital sources, or full-range scale for analog/motion. */
    float scale;
} le_input_binding;

/** Action consume mask: which contexts an event routes to. */
typedef enum le_consume_mask {
    LE_CONSUME_NONE = 0,
    LE_CONSUME_KEYBOARD = 1 << 0,
    LE_CONSUME_MOUSE = 1 << 1,
    LE_CONSUME_ALL = 0x7FFFFFFF
} le_consume_mask;

/* ---- raw device state (finalized per-frame snapshot) ---- */

/** Nonzero when the key is held (NULL engine or out-of-range ->
 *  0). Raw state ignores contexts (see action queries). */
LE_API int le_input_key_down(le_engine *engine, le_key key);
/** Edge: key transitioned up->down during the last input frame
 *  (auto-repeat never sets this — first press only). */
LE_API int le_input_key_pressed(le_engine *engine, le_key key);
/** Edge: key transitioned down->up during the last input frame. */
LE_API int le_input_key_released(le_engine *engine, le_key key);
LE_API int le_input_mouse_down(le_engine *engine,
                               le_mouse_button button);
LE_API int le_input_mouse_pressed(le_engine *engine,
                                  le_mouse_button button);
LE_API int le_input_mouse_released(le_engine *engine,
                                   le_mouse_button button);
/** Mouse position in client px of the focus window (out may be
 *  NULL; zeros for NULL engine). */
LE_API void le_input_mouse_position(le_engine *engine, float *out_x,
                                    float *out_y);
/** Motion accumulated since the previous input frame (zeros after
 *  the frame boundary is crossed). */
LE_API void le_input_mouse_delta(le_engine *engine, float *out_dx,
                                 float *out_dy);
/** Wheel detents accumulated since the previous input frame. */
LE_API void le_input_scroll_delta(le_engine *engine, float *out_x,
                                  float *out_y);
/** Current modifier snapshot. */
LE_API uint32_t le_input_mods(le_engine *engine);
/** Nonzero while any window of this engine has focus. */
LE_API int le_input_has_focus(le_engine *engine);
/** Request a cursor mode (best-effort platform request; returns
 *  the previous mode, NORMAL when unsupported). */
LE_API le_cursor_mode le_input_set_cursor_mode(le_engine *engine,
                                               le_cursor_mode mode);
LE_API le_cursor_mode le_input_get_cursor_mode(le_engine *engine);

/* UTF-8 text input (key identity stays physical; text arrives
 * here for consoles/fields — no UI system is built on it yet).
 * Reads one pending scalar per call (1) or 0 when empty; bytes
 * exclude NUL, buf always NUL-terminated on success. */
LE_API int le_input_read_text(le_engine *engine, char *buf,
                              uint32_t buf_cap, uint32_t *out_len);
LE_API uint32_t le_input_pending_text(le_engine *engine);

/* ---- gamepad foundation (backend-neutral API; platform
 * reporting PARTIAL in Phase 27: slots exist, injection drives
 * them, OS gamepads report disconnected until a platform backend
 * lands — never faked) ---- */
#define LE_GAMEPAD_MAX_SLOTS 8u
LE_API int le_gamepad_is_connected(le_engine *engine,
                                   uint32_t slot);
LE_API int le_gamepad_button_down(le_engine *engine, uint32_t slot,
                                  le_gamepad_button button);
LE_API int le_gamepad_button_pressed(le_engine *engine,
                                     uint32_t slot,
                                     le_gamepad_button button);
LE_API int le_gamepad_button_released(le_engine *engine,
                                      uint32_t slot,
                                      le_gamepad_button button);
/** Stick/trigger value after deadzone ([-1,+1] sticks, [0,1]
 *  triggers; 0 for NULL engine / bad slot / disconnected). */
LE_API float le_gamepad_axis_value(le_engine *engine, uint32_t slot,
                                   le_gamepad_axis axis);

/* ---- injection (tests/editor/MCP/replay foundation) ----
 * Injection feeds the SAME pending-event list as platform events
 * (identical state machine, identical edges). Deterministic: no
 * clock, no devices needed. */
LE_API le_result le_input_inject_key(le_engine *engine, le_key key,
                                     int down);
LE_API le_result le_input_inject_mouse_button(le_engine *engine,
                                              le_mouse_button button,
                                              int down);
LE_API le_result le_input_inject_mouse_move(le_engine *engine,
                                            float x, float y,
                                            float dx, float dy);
LE_API le_result le_input_inject_scroll(le_engine *engine, float dx,
                                        float dy);
LE_API le_result le_input_inject_text(le_engine *engine,
                                      const char *utf8);
LE_API le_result le_input_inject_focus(le_engine *engine,
                                       int focused);
LE_API le_result le_input_inject_gamepad_button(le_engine *engine,
                                                uint32_t slot,
                                                le_gamepad_button b,
                                                int down);
LE_API le_result le_input_inject_gamepad_axis(le_engine *engine,
                                              uint32_t slot,
                                              le_gamepad_axis axis,
                                              float value);

/* ---- actions (named, hashed, multi-binding aggregate) ---- */

/** Create an action (name copied, 1..127 bytes, nonempty). Repeat
 *  creation returns the SAME live action. */
LE_API le_result le_input_create_action(le_engine *engine,
                                        const char *name,
                                        le_input_action *out_action);
/** Resolve by name (1 + fill, 0 when absent; NULL-safe). */
LE_API int le_input_find_action(le_engine *engine, const char *name,
                                le_input_action *out_action);
LE_API le_result le_input_add_action_binding(
    le_engine *engine, const le_input_action *action,
    const le_input_binding *binding);
LE_API le_result le_input_remove_action_binding(
    le_engine *engine, const le_input_action *action,
    const le_input_binding *binding);
LE_API le_result le_input_clear_action_bindings(
    le_engine *engine, const le_input_action *action);
/** Query binding count (counting query when out NULL). */
LE_API le_result le_input_get_action_bindings(
    le_engine *engine, const le_input_action *action,
    le_input_binding *out, uint32_t cap, uint32_t *out_count);
/** Aggregate state over all bindings in active contexts. */
LE_API int le_input_action_down(le_engine *engine,
                                const le_input_action *action);
LE_API int le_input_action_pressed(le_engine *engine,
                                   const le_input_action *action);
LE_API int le_input_action_released(le_engine *engine,
                                    const le_input_action *action);

/* ---- axes (digital + analog, deadzone/scale/invert) ---- */

typedef struct le_axis_desc {
    /* Zero-init, then fill: name copied (1..127 bytes). */
    const char *name;
    float deadzone; /* |v| < deadzone -> 0 (default 0.15 gamepad) */
    float scale;    /* output multiplier (default 1) */
    int invert;     /* nonzero flips sign */
} le_axis_desc;

LE_API le_result le_input_create_axis(le_engine *engine,
                                      const le_axis_desc *desc,
                                      le_input_axis *out_axis);
LE_API int le_input_find_axis(le_engine *engine, const char *name,
                              le_input_axis *out_axis);
LE_API le_result le_input_add_axis_binding(
    le_engine *engine, const le_input_axis *axis,
    const le_input_binding *binding);
LE_API le_result le_input_remove_axis_binding(
    le_engine *engine, const le_input_axis *axis,
    const le_input_binding *binding);
LE_API le_result le_input_clear_axis_bindings(
    le_engine *engine, const le_input_axis *axis);
/** Sampled value in active contexts (0 when none/unbound). */
LE_API float le_input_axis_value(le_engine *engine,
                                 const le_input_axis *axis);

/* ---- contexts (lightweight named maps with priority) ---- */

/** Create/lookup a context (name copied, 1..63 bytes). Higher
 *  priority wins on overlap (default 0). */
LE_API le_result le_input_create_context(le_engine *engine,
                                         const char *name,
                                         int priority,
                                         le_input_context *out_ctx);
LE_API int le_input_find_context(le_engine *engine, const char *name,
                                 le_input_context *out_ctx);
LE_API le_result le_input_activate_context(
    le_engine *engine, const le_input_context *ctx);
LE_API le_result le_input_deactivate_context(
    le_engine *engine, const le_input_context *ctx);
LE_API int le_input_context_active(le_engine *engine,
                                   const le_input_context *ctx);
/** Bind an action/axis into a context (inactive contexts are
 *  skipped by queries). Actions/axes start GLOBAL (visible in
 *  every context); first context-bind narrows them. */
LE_API le_result le_input_context_bind_action(
    le_engine *engine, const le_input_context *ctx,
    const le_input_action *action);
LE_API le_result le_input_context_bind_axis(
    le_engine *engine, const le_input_context *ctx,
    const le_input_axis *axis);
/** Top active context may consume keyboard/mouse for the frame
 *  (gameplay below sees consumed domains as released). */
LE_API le_result le_input_context_set_consume(
    le_engine *engine, const le_input_context *ctx,
    uint32_t consume_mask);

/** Structured input stats (zeros for NULL; out may be NULL). */
typedef struct le_input_stats {
    uint32_t keys_down;
    uint32_t mouse_buttons_down;
    uint64_t events_ingested;
    uint32_t action_count;
    uint32_t axis_count;
    uint32_t active_contexts;
    uint32_t connected_gamepads;
    uint32_t pending_events;
} le_input_stats;

LE_API void le_input_get_stats(le_engine *engine,
                               le_input_stats *out_stats);

/* ---- engine/window attachment (multi-window policy) ----
 * An engine observes ZERO or more windows. The host attaches each
 * window whose queue feeds the engine (usually one; editors attach
 * one engine per viewport or share one engine across viewports).
 * The most-recently-gained focus wins for mouse position; close on
 * ANY attached window raises the quit request; resize-zero on the
 * focus window marks minimized. Detach on window destroy (the
 * engine never destroys windows). */
LE_API le_result le_engine_attach_window(le_engine *engine,
                                         lc_window *window);
LE_API le_result le_engine_detach_window(le_engine *engine,
                                         lc_window *window);

/* ------------------------------------------------------------------
 * Engine time (Phase 27): monotonic, engine-owned, Lua-consumed.
 *
 * Source: real runtime advances from lc_clock_now (ns, monotonic);
 * tests advance with explicit deltas (le_engine_step) through the
 * SAME state machine. Elapsed accumulates in double; per-frame dt
 * is float. NaN/Inf/negative time_scale is rejected (scale stays
 * unchanged); negative deltas clamp to 0.
 * ------------------------------------------------------------------ */

typedef struct le_time_stats {
    uint64_t frame_index;
    double raw_delta;
    double scaled_delta;
    double elapsed;
    double unscaled_elapsed;
    float fixed_delta;
    uint32_t fixed_steps;
    uint32_t fixed_steps_dropped;
    float time_scale;
} le_time_stats;

LE_API void le_time_get_stats(le_engine *engine,
                              le_time_stats *out_stats);
LE_API double le_time_delta(le_engine *engine);
LE_API double le_time_unscaled_delta(le_engine *engine);
LE_API double le_time_elapsed(le_engine *engine);
LE_API double le_time_unscaled_elapsed(le_engine *engine);
LE_API uint64_t le_time_frame_index(le_engine *engine);
/** Set scale (1 normal, 0.5 slow, 0 paused). Rejects NaN/Inf/
 *  negative (returns INVALID_ARGUMENT, scale unchanged). */
LE_API le_result le_time_set_scale(le_engine *engine, float scale);
LE_API float le_time_get_scale(le_engine *engine);
/** Max simulation delta per frame (default 0.25 s; 0 disables the
 *  clamp... raw measurement stays visible in stats). */
LE_API le_result le_time_set_max_delta(le_engine *engine,
                                       float max_delta);
LE_API float le_time_get_max_delta(le_engine *engine);
/** Fixed-step interval (default 1/60; 0 disables fixed_update;
 *  >1.0 clamps to 1.0 like the Phase 26 contract). */
LE_API le_result le_time_set_fixed_delta(le_engine *engine,
                                         float fixed_delta);
LE_API float le_time_get_fixed_delta(le_engine *engine);
/** Queue one fixed step while paused (debug single-step). */
LE_API le_result le_time_request_single_step(le_engine *engine);

/* ------------------------------------------------------------------
 * Frame + application + world lifecycle (Phase 27).
 *
 * Explicit contract (host owns the loop):
 *   le_engine_begin_frame(engine)   // poll+ingest input, time
 *   le_engine_update(engine, world) // sim + scripts (per world)
 *   ... render trio (existing API) ...
 *   le_engine_end_frame(engine)     // edge cleanup
 * or one call: le_engine_frame(engine, worlds, count).
 * le_world_update(world, dt) keeps its Phase 24-26 contract as a
 * thin wrapper (explicit-dt stepping for tests).
 * ------------------------------------------------------------------ */

typedef enum le_app_state {
    LE_APP_RUNNING = 0,
    LE_APP_QUIT_REQUESTED = 1
} le_app_state;

LE_API le_result le_engine_begin_frame(le_engine *engine);
LE_API le_result le_engine_update(le_engine *engine,
                                  le_world *world);
LE_API le_result le_engine_end_frame(le_engine *engine);
/** Convenience over begin/update(each world)/end. worlds may be
 *  NULL with count 0 (input+time still advance). */
LE_API le_result le_engine_frame(le_engine *engine,
                                 le_world **worlds,
                                 uint32_t world_count);
/** Explicit-delta stepping (deterministic tests): same state
 *  machine as the clock path with caller-supplied dt. */
LE_API le_result le_engine_step(le_engine *engine, le_world *world,
                                float dt);
/** Quit request (window close feeds this; host may set/clear).
 *  The engine never calls exit(). */
LE_API le_result le_engine_request_quit(le_engine *engine);
LE_API le_result le_engine_cancel_quit(le_engine *engine);
LE_API le_app_state le_engine_app_state(le_engine *engine);
/** Focus/minimize observation (from window events). */
LE_API int le_engine_has_focus(le_engine *engine);
LE_API int le_engine_is_minimized(le_engine *engine);
/** Per-world pause (independent of global time scale; editor can
 *  hold the game world while its own world runs). */
LE_API le_result le_world_set_paused(le_world *world, int paused);
LE_API int le_world_is_paused(const le_world *world);

/* ------------------------------------------------------------------
 * Lua scripting runtime (Phase 26): an interpreted backend over the
 * VM-independent script lifecycle. The vendored backend sources live
 * in third_party/lua and are built as a private static library; NO
 * backend VM type crosses this header. The engine object model
 * never depends on Lua: le_object / le_asset / le_scene handles
 * stay the authoritative identity, and the lifecycle below is
 * documented so a future native/AOT backend can implement the same
 * contract (see docs/SCRIPT_AOT_ARCHITECTURE.md).
 *
 * Threading: the runtime is owned by its le_engine and driven
 * exclusively from the world's owning thread (le_world_update).
 * Lua states are never shared across threads.
 * ------------------------------------------------------------------ */

/** Find the first live object with an exact name match (slot order;
 *  1 when found with *out_object filled, 0 otherwise; out may be
 *  NULL for a presence query). Name is convenience, NOT identity. */
LE_API int le_world_find_by_name(le_world *world, const char *name,
                                 le_object *out_object);

/** Find a live asset by persistent ID (1 when a READY asset with
 *  this ID exists, 0 otherwise). Used by script bindings and tools;
 *  instantiation resolves internally. */
LE_API int le_asset_find_by_id(le_engine *engine,
                               const le_asset_id *id,
                               le_asset *out_asset);

/* Script source description (zero-init, then fill). `source` +
 * `size` carry UTF-8 Lua bytes (copied in); `path_hint` names the
 * origin for diagnostics/module identity (normalized like every
 * asset source; may be "" for ad-hoc snippets). Persistent ID is
 * the FNV-1a content hash (identical bytes = identical ID). */
typedef struct le_script_asset_desc {
    const char *source;
    size_t size;
    const char *path_hint;
} le_script_asset_desc;

/** Create a READY script asset (compiles immediately: syntax errors
 *  fail here with LE_ERROR_PARSE and create NOTHING — transactional,
 *  registry unchanged).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args, empty
 *         source), LE_ERROR_PARSE (syntax error),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_asset_create_script(le_engine *engine,
                                        const le_script_asset_desc *desc,
                                        le_asset *out_asset);

/** Load a script file into the registry (explicit path, deduped by
 *  canonical path like glTF: repeat loads return the SAME handles).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_MISSING_ASSET (file absent),
 *         LE_ERROR_PARSE (syntax error — registry unchanged),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_asset_load_script(le_engine *engine,
                                      const char *path,
                                      le_asset *out_asset);

/** Replace a script asset's source bytes (recompiles first:
 *  failure keeps the OLD working source — transactional).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args, empty),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_WRONG_ASSET_TYPE,
 *         LE_ERROR_PARSE (old source kept),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_script_asset_set_source(le_engine *engine,
                                            const le_asset *asset,
                                            const char *source,
                                            size_t size);

/** Reload a script asset from its CURRENT source bytes (recompile +
 *  swap; live instances adopt the new code, keep exported property
 *  values and instance state, and do NOT re-run start()). Failure
 *  leaves the old code live (transactional).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine/asset),
 *         LE_ERROR_STALE_ASSET, LE_ERROR_WRONG_ASSET_TYPE,
 *         LE_ERROR_PARSE (old code kept).
 */
LE_API le_result le_script_reload(le_engine *engine,
                                  const le_asset *asset);

/** Attach (or replace) the single script component on a live object.
 *  The asset must be a live READY LE_ASSET_SCRIPT on this world's
 *  engine. The instance starts on the next le_world_update once
 *  effectively enabled (never twice; scene-instantiated scripts
 *  start in instantiation order).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE,
 *         LE_ERROR_STALE_ASSET, LE_ERROR_WRONG_ASSET_TYPE,
 *         LE_ERROR_MISSING_ASSET (script not READY),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_object_add_script(le_world *world,
                                      const le_object *object,
                                      const le_asset *script);

/** Remove a script (fires destroy() iff start() ran; missing
 *  component is success/no-op).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world/object),
 *         LE_ERROR_WRONG_WORLD, LE_ERROR_STALE_HANDLE.
 */
LE_API le_result le_object_remove_script(le_world *world,
                                         const le_object *object);

/** Copy out the script asset handle (LE_ASSET_INVALID for
 *  NULL/stale/missing; out may be NULL). Returns 1 when present, 0
 *  otherwise. */
LE_API int le_object_get_script(const le_world *world,
                                const le_object *object,
                                le_asset *out_script);

/** Nonzero when the object's script instance is currently failed
 *  (a lifecycle callback errored and the instance was disabled per
 *  the error policy). 0 for NULL/stale/scriptless/healthy. */
LE_API int le_object_script_failed(const le_world *world,
                                   const le_object *object);

/** Read one exported property value (1 when present, 0 for
 *  NULL/stale/scriptless/unknown-name; out may be NULL for a
 *  presence query). Values reflect live instance state. */
LE_API int le_script_get_property(le_world *world,
                                  const le_object *object,
                                  const char *name,
                                  le_script_property *out_prop);

/** Write one exported property value (type must match the export
 *  declaration; asset values must be live handles on this engine).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL args, unknown
 *         name, type mismatch, bad asset), LE_ERROR_WRONG_WORLD,
 *         LE_ERROR_STALE_HANDLE, LE_ERROR_STALE_ASSET.
 */
LE_API le_result le_script_set_property(le_world *world,
                                        const le_object *object,
                                        const le_script_property *prop);

/** List exported property names for an instance (up to `capacity`
 *  entries; always reports the full count in `out_count`; either
 *  out pointer may be NULL). Returns 1 when a script is present, 0
 *  otherwise. */
LE_API int le_script_list_properties(
    le_world *world, const le_object *object,
    le_script_property *out_props, uint32_t capacity,
    uint32_t *out_count);

/* Script runtime configuration (zero-init for defaults). Applied
 * per engine; worlds inherit fixed-step settings individually via
 * le_script_set_fixed_step. */
typedef void (*le_script_log_fn)(const char *message, void *user);

typedef struct le_script_config {
    uint64_t max_instructions_per_callback;
    uint64_t memory_budget_bytes;
    char script_root[1024];
    le_script_log_fn log_fn;
    void *log_user;
} le_script_config;

/** Configure the engine script runtime (creates it lazily; safe to
 *  call before any script exists; NULL desc resets to defaults).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_script_configure(le_engine *engine,
                                     const le_script_config *desc);

/** Add a module search root (project-relative; require() resolves
 *  beneath roots only — `..` escapes rejected). Roots persist for
 *  the engine lifetime.
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL engine/path),
 *         LE_ERROR_OUT_OF_MEMORY.
 */
LE_API le_result le_script_add_search_path(le_engine *engine,
                                           const char *path);

/** Per-world fixed-step schedule (fixed_update runs on this clock;
 *  0 disables fixed_update while update() still runs). max_steps
 *  caps catch-up per frame (spiral-of-death guard).
 *
 * @return LE_SUCCESS, LE_ERROR_INVALID_ARGUMENT (NULL world,
 *         negative dt, max_steps 0 with dt nonzero).
 */
LE_API le_result le_script_set_fixed_step(le_world *world,
                                          float fixed_dt,
                                          uint32_t max_steps);

/** Structured script diagnostics (plain counts; zeros for NULL).
 *  script_bytes/peak track the engine script allocator; callbacks
 *  counts the current frame (reset each le_world_update). */
typedef struct le_script_stats {
    uint32_t script_assets;
    uint32_t script_instances;
    uint32_t active_instances;
    uint32_t disabled_instances;
    uint32_t failed_instances;
    uint32_t errors_total;
    uint64_t callbacks_this_frame;
    uint64_t script_bytes;
    uint64_t script_peak_bytes;
} le_script_stats;

/** Copy out engine-wide script stats (zeros for NULL; out may be
 *  NULL). */
LE_API void le_script_get_stats(const le_engine *engine,
                                le_script_stats *out_stats);

/** Last script error snapshot (message + traceback + origin; "" /
 *  LE_OBJECT_INVALID when none yet). Borrowed strings valid until
 *  the next script error or engine shutdown. */
typedef struct le_script_error {
    const char *message;
    const char *traceback;
    const char *script_source;
    const char *callback;
    le_object object;
    int has_object;
} le_script_error;

/** Copy out the most recent script error (zeros for NULL engine or
 *  no error yet; out may be NULL). */
LE_API void le_script_get_last_error(const le_engine *engine,
                                     le_script_error *out_error);

#ifdef __cplusplus
}
#endif

#endif /* LUMA_ENGINE_H */
