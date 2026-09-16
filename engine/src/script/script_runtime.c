/*
 * Luma script runtime (Phase 26): one Lua state per le_engine,
 * created lazily on first script use. Owns configuration, the
 * allocator wrapper (accounting + optional budget), the
 * instruction-count hook (deterministic runaway protection), the
 * sandboxed standard library set, the project-rooted require(),
 * and print routing. Lua failures never propagate as C++-style
 * unwinding past the engine: every entry point uses pcall.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

/* ------------------------------------------------------------------
 * Allocator wrapper: accounting + peak + optional hard budget.
 * ------------------------------------------------------------------ */

static void *le_lua_alloc(void *ud, void *ptr, size_t osize,
                          size_t nsize) {
    le_script_runtime *rt = (le_script_runtime *)ud;

    if (nsize == 0) {
        if (ptr != NULL) {
            if (osize <= rt->lua_bytes) {
                rt->lua_bytes -= osize;
            } else {
                rt->lua_bytes = 0;
            }
            free(ptr);
        }
        return NULL;
    }
    if (rt->memory_budget > 0) {
        size_t want = rt->lua_bytes;

        if (nsize > osize) {
            want += nsize - osize;
        }
        if (want > rt->memory_budget) {
            rt->allocator_failed = 1;
            return NULL;
        }
    }
    {
        void *fresh = realloc(ptr, nsize);

        if (fresh == NULL) {
            return NULL;
        }
        if (nsize > osize) {
            rt->lua_bytes += nsize - osize;
        } else if (osize > nsize) {
            size_t drop = osize - nsize;

            rt->lua_bytes =
                (drop <= rt->lua_bytes) ? rt->lua_bytes - drop : 0;
        }
        if (rt->lua_bytes > rt->lua_peak) {
            rt->lua_peak = rt->lua_bytes;
        }
        rt->alloc_count++;
        return fresh;
    }
}

/* ------------------------------------------------------------------
 * Instruction hook: deterministic per-callback budgets (count-based,
 * never wall-clock). The hook fires every GRANULE instructions and
 * aborts via lua_error when the budget is spent.
 * ------------------------------------------------------------------ */

static void le_lua_hook(lua_State *L, lua_Debug *ar) {
    le_script_runtime *rt = NULL;

    (void)ar;
    lua_getfield(L, LUA_REGISTRYINDEX, "le_runtime");
    rt = (le_script_runtime *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (rt == NULL || rt->L != L) {
        return;
    }
    if (rt->instructions_left <= LE_SCRIPT_HOOK_GRANULE) {
        rt->instructions_left = 0;
        rt->budget_exceeded = 1;
        luaL_error(L, "instruction budget exceeded");
        return;
    }
    rt->instructions_left -= LE_SCRIPT_HOOK_GRANULE;
}

static void le_hook_begin(le_script_runtime *rt) {
    rt->instructions_left =
        (rt->max_instructions > 0) ? rt->max_instructions
                                   : LE_SCRIPT_DEFAULT_INSTRUCTIONS;
    rt->budget_exceeded = 0;
    lua_sethook(rt->L, le_lua_hook, LUA_MASKCOUNT,
                LE_SCRIPT_HOOK_GRANULE);
}

static void le_hook_end(le_script_runtime *rt) {
    lua_sethook(rt->L, NULL, 0, 0);
    (void)rt;
}

/* Hook helpers shared with script_lua.c (same library, one
 * backend): arm/disarm the per-callback instruction budget. */
void le_script_hook_begin(le_script_runtime *rt);
void le_script_hook_end(le_script_runtime *rt);

void le_script_hook_begin(le_script_runtime *rt) {
    le_hook_begin(rt);
}

void le_script_hook_end(le_script_runtime *rt) {
    le_hook_end(rt);
}

/* ------------------------------------------------------------------
 * print routing: global print() -> engine log callback (default:
 * stderr with a [script] tag). Never raw unfiltered I/O policy.
 * ------------------------------------------------------------------ */

static int le_lua_print(lua_State *L) {
    le_script_runtime *rt = NULL;
    int n = lua_gettop(L);
    int i;
    luaL_Buffer b;
    char *out = NULL;
    size_t len = 0;

    lua_getfield(L, LUA_REGISTRYINDEX, "le_runtime");
    rt = (le_script_runtime *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++) {
        size_t l = 0;
        const char *s = NULL;

        if (luaL_callmeta(L, i, "__tostring")) {
            s = lua_tostring(L, -1);
            l = (s != NULL) ? strlen(s) : 0;
            luaL_addlstring(&b, (s != NULL) ? s : "?", l);
            lua_pop(L, 1);
        } else if (lua_type(L, i) == LUA_TSTRING) {
            s = lua_tolstring(L, i, &l);
            luaL_addlstring(&b, s, l);
        } else {
            luaL_tolstring(L, i, &l);
            s = lua_tolstring(L, -1, &l);
            luaL_addlstring(&b, s, l);
            lua_pop(L, 1);
        }
        if (i < n) {
            luaL_addchar(&b, '\t');
        }
    }
    luaL_pushresult(&b);
    out = (char *)lua_tostring(L, -1);
    len = (out != NULL) ? strlen(out) : 0;
    (void)len;
    if (rt != NULL) {
        le_script_log(rt, (out != NULL) ? out : "");
    }
    return 0;
}

void le_script_log(le_script_runtime *rt, const char *message) {
    if (rt != NULL && rt->log_fn != NULL) {
        rt->log_fn((message != NULL) ? message : "", rt->log_user);
        return;
    }
    fprintf(stderr, "[script] %s\n",
            (message != NULL) ? message : "");
}

static void le_default_log(const char *message, void *user) {
    (void)user;
    fprintf(stderr, "[script] %s\n",
            (message != NULL) ? message : "");
}

/* ------------------------------------------------------------------
 * Sandboxed require(): project roots only, `..` escapes rejected,
 * per-engine module cache (registry table "le_modules").
 * ------------------------------------------------------------------ */

static int le_require_sandboxed(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    le_script_runtime *rt = NULL;
    char rel[1024];
    size_t i = 0;
    size_t o = 0;

    lua_getfield(L, LUA_REGISTRYINDEX, "le_runtime");
    rt = (le_script_runtime *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (rt == NULL) {
        return luaL_error(L, "no script runtime");
    }
    if (name == NULL || name[0] == '\0') {
        return luaL_error(L, "bad module name");
    }
    /* Dots become separators (scripts.combat -> scripts/combat);
     * anything else must be [A-Za-z0-9_./]. */
    while (name[i] != '\0' && o + 1 < sizeof(rel)) {
        char c = name[i];

        if (c == '.') {
            rel[o++] = '/';
        } else if ((c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '_' ||
                   c == '/') {
            rel[o++] = c;
        } else {
            return luaL_error(L, "bad module name");
        }
        i++;
    }
    if (name[i] != '\0') {
        return luaL_error(L, "module name too long");
    }
    rel[o] = '\0';
    /* Module cache hit? */
    lua_getfield(L, LUA_REGISTRYINDEX, "le_modules");
    lua_getfield(L, -1, rel);
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);
    /* Resolve beneath roots (script_root first, then extras). */
    {
        char tried[4096];
        int found = 0;
        uint32_t r;
        char cand[2048];

        for (r = 0; r <= rt->search_path_count && !found; r++) {
            const char *root = (r == 0) ? rt->script_root
                                        : rt->search_paths[r - 1];
            FILE *f = NULL;
            long len = 0;
            char *bytes = NULL;
            size_t got = 0;

            if (root[0] == '\0') {
                continue;
            }
            snprintf(cand, sizeof(cand), "%s/%s.lua", root, rel);
            /* Canonicalize the JOIN (lexical) and require it to
             * stay beneath the root — `..` escapes rejected. */
            {
                char norm[2048];
                char nroot[1024];

                if (!le_normalize_source(cand, norm,
                                         sizeof(norm)) ||
                    !le_normalize_source(root, nroot,
                                         sizeof(nroot))) {
                    continue;
                }
                if (strncmp(norm, nroot, strlen(nroot)) != 0 ||
                    (norm[strlen(nroot)] != '/' &&
                     norm[strlen(nroot)] != '\0')) {
                    continue;
                }
                snprintf(tried, sizeof(tried), "%s", norm);
            }
#ifdef _MSC_VER
            if (fopen_s(&f, tried, "rb") != 0 || f == NULL) {
                continue;
            }
#else
            f = fopen(tried, "rb");
            if (f == NULL) {
                continue;
            }
#endif
            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                continue;
            }
            len = ftell(f);
            if (len < 0 || len > (long)LE_SCRIPT_MAX_SOURCE) {
                fclose(f);
                continue;
            }
            if (fseek(f, 0, SEEK_SET) != 0) {
                fclose(f);
                continue;
            }
            bytes = (char *)malloc((size_t)len + 1u);
            if (bytes == NULL) {
                fclose(f);
                return luaL_error(L, "out of memory");
            }
            got = fread(bytes, 1, (size_t)len, f);
            fclose(f);
            if (got != (size_t)len) {
                free(bytes);
                continue;
            }
            le_hook_begin(rt);
            {
                int rc = luaL_loadbufferx(L, bytes, got, tried,
                                          "t");

                free(bytes);
                if (rc != LUA_OK) {
                    const char *msg = lua_tostring(L, -1);

                    le_hook_end(rt);
                    return luaL_error(L,
                                      "module '%s': %s",
                                      rel,
                                      (msg != NULL) ? msg : "?");
                }
                rc = lua_pcall(L, 0, 1, 0);
                le_hook_end(rt);
                if (rc != LUA_OK) {
                    const char *msg = lua_tostring(L, -1);

                    return luaL_error(L,
                                      "module '%s': %s",
                                      rel,
                                      (msg != NULL) ? msg : "?");
                }
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_pushboolean(L, 1);
                }
            }
            /* Cache + return. Stack: modules, result. */
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, rel);
            found = 1;
        }
        if (!found) {
            return luaL_error(L, "module '%s' not found", rel);
        }
        /* Stack: modules, result -> drop modules table. */
        lua_remove(L, -2);
        return 1;
    }
}

/* ------------------------------------------------------------------
 * Runtime lifecycle.
 * ------------------------------------------------------------------ */

le_script_runtime *le_lua_current_runtime(lua_State *L) {
    le_script_runtime *rt = NULL;

    if (L == NULL) {
        return NULL;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "le_runtime");
    rt = (le_script_runtime *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return rt;
}

static void le_open_sandboxed(lua_State *L) {
    /* Deliberate subset: base MINUS dofile/loadfile (filesystem
     * code loading goes through require()/assets), table, string,
     * math, utf8, coroutine. EXCLUDED: io, os (clocks stay
     * engine-provided via dt), package (custom require below),
     * debug (tracebacks via luaL_traceback, no debug lib needed).
     * Documented in docs/LUA_SCRIPTING_ARCHITECTURE.md. */
    static const luaL_Reg libs[] = {
        { "_G", luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_COLIBNAME, luaopen_coroutine },
        { NULL, NULL },
    };
    const luaL_Reg *lib = libs;

    for (; lib->func != NULL; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    /* Strip filesystem-y base members (keep load? NO — load()
     * compiles arbitrary strings; modules use require(); script
     * bodies come from assets. load/loadstring/dofile/loadfile/
     * collectgarbage (GC policy is engine-owned) go. print is
     * rerouted below. */
    lua_getglobal(L, "_G");
    lua_pushnil(L);
    lua_setfield(L, -2, "dofile");
    lua_pushnil(L);
    lua_setfield(L, -2, "loadfile");
    lua_pushnil(L);
    lua_setfield(L, -2, "load");
    lua_pushnil(L);
    lua_setfield(L, -2, "collectgarbage");
    lua_pushcfunction(L, le_lua_print);
    lua_setfield(L, -2, "print");
    lua_pushcfunction(L, le_require_sandboxed);
    lua_setfield(L, -2, "require");
    lua_pop(L, 1);
}

le_result le_script_runtime_ensure(le_engine *engine) {
    le_script_runtime *rt;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->script_runtime != NULL) {
        if (engine->script_runtime->L == NULL ||
            engine->script_runtime->backend == NULL) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        return LE_SUCCESS;
    }
    rt = (le_script_runtime *)calloc(1, sizeof(*rt));
    if (rt == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    rt->backend = &le_lua_backend_ops;
    rt->max_instructions = LE_SCRIPT_DEFAULT_INSTRUCTIONS;
    rt->log_fn = le_default_log;
    rt->L = lua_newstate(le_lua_alloc, rt);
    if (rt->L == NULL) {
        free(rt);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    /* Registry anchors: runtime pointer, module cache. */
    lua_pushlightuserdata(rt->L, rt);
    lua_setfield(rt->L, LUA_REGISTRYINDEX, "le_runtime");
    lua_newtable(rt->L);
    lua_setfield(rt->L, LUA_REGISTRYINDEX, "le_modules");
    le_open_sandboxed(rt->L);
    le_lua_register_world(rt->L);
    le_lua_register_object(rt->L);
    le_lua_register_input(rt->L);
    engine->script_runtime = rt;
    return LE_SUCCESS;
}

void le_script_runtime_destroy(le_engine *engine) {
    if (engine == NULL || engine->script_runtime == NULL) {
        return;
    }
    lua_close(engine->script_runtime->L);
    free(engine->script_runtime);
    engine->script_runtime = NULL;
}

void le_script_release_chunk_token(le_engine *engine, int chunk) {
    if (engine == NULL || engine->script_runtime == NULL ||
        chunk < 0) {
        return;
    }
    if (engine->script_runtime->backend != NULL &&
        engine->script_runtime->backend->release_chunk != NULL) {
        engine->script_runtime->backend->release_chunk(
            engine->script_runtime, chunk);
    }
}

void le_script_record_error(le_script_runtime *rt, const char *callback,
                            le_world *world, const le_object *obj,
                            const char *source,
                            const char *message) {
    const char *msg = (message != NULL) ? message : "unknown error";
    size_t n;

    if (rt == NULL) {
        return;
    }
    n = strlen(msg);
    if (n >= sizeof(rt->last_message)) {
        n = sizeof(rt->last_message) - 1;
    }
    memcpy(rt->last_message, msg, n);
    rt->last_message[n] = '\0';
    /* Traceback via luaL_traceback (needs no debug lib). Runs on
     * whatever stack depth dispatch left (fire() hard-balances to
     * empty, plus a bounded push/pop here). */
    if (rt->L != NULL) {
        int top = lua_gettop(rt->L);

        luaL_traceback(rt->L, rt->L, NULL, 1);
        {
            const char *tb = lua_tostring(rt->L, -1);

            if (tb != NULL) {
                size_t t = strlen(tb);

                if (t >= sizeof(rt->last_traceback)) {
                    t = sizeof(rt->last_traceback) - 1;
                }
                memcpy(rt->last_traceback, tb, t);
                rt->last_traceback[t] = '\0';
            } else {
                rt->last_traceback[0] = '\0';
            }
        }
        lua_settop(rt->L, top);
    } else {
        rt->last_traceback[0] = '\0';
    }
    snprintf(rt->last_callback, sizeof(rt->last_callback), "%s",
             (callback != NULL) ? callback : "?");
    snprintf(rt->last_source, sizeof(rt->last_source), "%s",
             (source != NULL) ? source : "");
    if (obj != NULL && world != NULL) {
        rt->last_object = *obj;
        rt->has_last_object = 1;
    } else {
        rt->has_last_object = 0;
    }
    (void)world;
    rt->has_last_error = 1;
    rt->errors_total++;
    {
        char line[640];

        snprintf(line, sizeof(line),
                 "script error [%s] %s: %s",
                 rt->last_callback,
                 (rt->last_source[0] != '\0') ? rt->last_source
                                              : "<script>",
                 rt->last_message);
        le_script_log(rt, line);
    }
}

/* --- public config/diagnostics API (implemented here; dispatch in
 * script_step.c, assets in script_asset.c, props in
 * script_lua.c) --- */

le_result le_script_configure(le_engine *engine,
                              const le_script_config *desc) {
    le_result rc;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_script_runtime_ensure(engine);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    if (desc == NULL) {
        /* Reset to defaults (keeps search paths + log target). */
        engine->script_runtime->max_instructions =
            LE_SCRIPT_DEFAULT_INSTRUCTIONS;
        engine->script_runtime->memory_budget = 0;
        return LE_SUCCESS;
    }
    if (desc->max_instructions_per_callback != 0) {
        engine->script_runtime->max_instructions =
            desc->max_instructions_per_callback;
    }
    engine->script_runtime->memory_budget =
        desc->memory_budget_bytes;
    if (desc->script_root[0] != '\0') {
        char norm[1024];

        if (!le_normalize_source(desc->script_root, norm,
                                 sizeof(norm))) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        memcpy(engine->script_runtime->script_root, norm,
               sizeof(norm));
    }
    if (desc->log_fn != NULL) {
        engine->script_runtime->log_fn = desc->log_fn;
        engine->script_runtime->log_user = desc->log_user;
    }
    return LE_SUCCESS;
}

le_result le_script_add_search_path(le_engine *engine,
                                    const char *path) {
    le_result rc;
    le_script_runtime *rt;
    char norm[1024];

    if (engine == NULL || path == NULL || path[0] == '\0') {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_script_runtime_ensure(engine);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    rt = engine->script_runtime;
    if (rt->search_path_count >= LE_SCRIPT_MAX_SEARCH_PATHS) {
        return LE_ERROR_OVERFLOW;
    }
    if (!le_normalize_source(path, norm, sizeof(norm))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memcpy(rt->search_paths[rt->search_path_count], norm,
           sizeof(norm));
    rt->search_path_count++;
    return LE_SUCCESS;
}

le_result le_script_set_fixed_step(le_world *world, float fixed_dt,
                                   uint32_t max_steps) {
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!(fixed_dt == fixed_dt) || fixed_dt < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (fixed_dt > 0.0f && max_steps == 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (fixed_dt > 1.0f) {
        fixed_dt = 1.0f;
    }
    world->script_fixed_dt = fixed_dt;
    world->script_max_steps =
        (max_steps == 0) ? LE_SCRIPT_DEFAULT_MAX_STEPS : max_steps;
    return LE_SUCCESS;
}

uint32_t le_script_world_instance_count(const le_world *world,
                                        int *out_active,
                                        int *out_disabled,
                                        int *out_failed) {
    uint32_t i;
    int active = 0;
    int disabled = 0;
    int failed = 0;

    if (out_active != NULL) {
        *out_active = 0;
    }
    if (out_disabled != NULL) {
        *out_disabled = 0;
    }
    if (out_failed != NULL) {
        *out_failed = 0;
    }
    if (world == NULL) {
        return 0;
    }
    for (i = 0; i < world->script_count; i++) {
        const le_script_entry *e = &world->scripts[i];

        if (e->failed) {
            failed++;
        } else if (!e->started || e->pending_start) {
            disabled++;
        } else {
            uint32_t slot = e->slot;

            if (slot < world->capacity && world->slots[slot].alive) {
                le_object h;

                h.index = slot;
                h.generation = world->slots[slot].generation;
                h.world_tag = world->tag;
                if (le_object_is_effectively_enabled(world, &h)) {
                    active++;
                } else {
                    disabled++;
                }
            } else {
                disabled++;
            }
        }
    }
    if (out_active != NULL) {
        *out_active = active;
    }
    if (out_disabled != NULL) {
        *out_disabled = disabled;
    }
    if (out_failed != NULL) {
        *out_failed = failed;
    }
    return world->script_count;
}

void le_script_get_stats(const le_engine *engine,
                         le_script_stats *out_stats) {
    const le_world *w;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (engine == NULL) {
        return;
    }
    {
        uint32_t i;

        for (i = 0; i < engine->asset_capacity; i++) {
            const le_asset_slot *s = &engine->assets[i];

            if (s->alive && s->type == LE_ASSET_SCRIPT) {
                out_stats->script_assets++;
            }
        }
    }
    for (w = engine->worlds; w != NULL; w = w->next) {
        int a = 0;
        int d = 0;
        int f = 0;
        uint32_t n = le_script_world_instance_count(w, &a, &d, &f);

        out_stats->script_instances += n;
        out_stats->active_instances += (uint32_t)a;
        out_stats->disabled_instances += (uint32_t)d;
        out_stats->failed_instances += (uint32_t)f;
    }
    if (engine->script_runtime != NULL) {
        out_stats->errors_total =
            engine->script_runtime->errors_total;
        out_stats->callbacks_this_frame =
            engine->script_runtime->callbacks_this_frame;
        out_stats->script_bytes =
            (uint64_t)engine->script_runtime->lua_bytes;
        out_stats->script_peak_bytes =
            (uint64_t)engine->script_runtime->lua_peak;
    }
}

void le_script_get_last_error(const le_engine *engine,
                              le_script_error *out_error) {
    if (out_error == NULL) {
        return;
    }
    memset(out_error, 0, sizeof(*out_error));
    out_error->message = "";
    out_error->traceback = "";
    out_error->script_source = "";
    out_error->callback = "";
    out_error->object = LE_OBJECT_INVALID;
    if (engine == NULL || engine->script_runtime == NULL ||
        !engine->script_runtime->has_last_error) {
        return;
    }
    out_error->message = engine->script_runtime->last_message;
    out_error->traceback = engine->script_runtime->last_traceback;
    out_error->script_source = engine->script_runtime->last_source;
    out_error->callback = engine->script_runtime->last_callback;
    if (engine->script_runtime->has_last_object) {
        out_error->object = engine->script_runtime->last_object;
        out_error->has_object = 1;
    }
}
