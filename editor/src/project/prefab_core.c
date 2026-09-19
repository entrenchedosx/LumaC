/* Phase 32 prefab foundation: reusable authored object-subtree
 * assets (`.luprefab`, `LUMA_PREFAB 1`).
 *
 * Format (reuses the scene record vocabulary; same writer
 * discipline -- `%.9g`, escapes, 32-hex IDs, deterministic order):
 *   LUMA_PREFAB 1
 *   prefab <uuid>            (prefab asset UUID, authoring identity)
 *   object <local-uuid>
 *     ...same lines as scenes (name/enabled/parent/TRS/renderable/
 *         camera/light/shadow/script/sprop/rigid_body/collider/
 *         animator/character/end)...
 *   prefab_root <local-uuid>
 * Rules: exactly one prefab line (first, after header), >=1
 * object, exactly one prefab_root naming a known local ID, parent
 * links resolve within the payload (no external parents), no
 * nested-prefab references (any second `prefab` line rejected),
 * duplicate local IDs rejected, unknown components rejected (same
 * fail-closed list as scenes), unknown fields tolerated. Canonical
 * bytes stable across load->save->load->save (ID-sorted records,
 * fixed field order).
 *
 * Identity levels (three, never conflated):
 *   prefab asset UUID  (which prefab)
 *   prefab-local UUID  (which object within the prefab)
 *   runtime le_object  (which live instance object; fresh handles
 *                       per instantiation, salted renderer keys)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

#define LED_PREFAB_MAGIC "LUMA_PREFAB 1"
#define LED_PREFAB_MAX_TEXT (64u * 1024u * 1024u)

typedef struct led_prefab_node {
    le_scene_object_id local;
    le_scene_object_id parent_local;
    int has_parent;
    char name[128];
    int enabled;
    float position[3];
    float rotation[4];
    float scale[3];
    int has_asset_renderable;
    le_asset_id mesh_id;
    le_asset_id material_id;
    int casts_shadow;
    int receives_shadow;
    int visible;
    int has_camera;
    le_camera_desc camera;
    int has_light;
    le_light_desc light;
    int has_script;
    le_asset_id script_id;
    le_script_property script_props[16];
    uint32_t script_prop_count;
    int has_body;
    le_rigid_body_desc body;
    int has_collider;
    le_collider_desc collider;
    int has_animator;
    le_asset_id skeleton_id;
    le_asset_id clip_id;
    int animator_autoplay;
    int animator_loop;
    float animator_speed;
    float animator_start_time;
    int has_character;
    le_character_desc character;
} led_prefab_node;

/* Membership test: object under root (root inclusive). */
static int led_prefab_member(le_world *w, const le_object *o,
                             const le_object *root,
                             uint32_t bound) {
    le_object walk = *o;
    uint32_t hops = 0;
    le_object p = LE_OBJECT_INVALID;

    if (o->index == root->index && o->generation ==
            root->generation &&
        o->world_tag == root->world_tag) {
        return 1;
    }
    while (hops++ < bound &&
           le_object_get_parent(w, &walk, &p)) {
        if (p.index == root->index &&
            p.generation == root->generation &&
            p.world_tag == root->world_tag) {
            return 1;
        }
        walk = p;
    }
    return 0;
}

/* Collect the subtree (root + descendants, ascending slot) with
 * live authoring snapshots. Returns NULL on OOM/empty/stale.
 * Rejects subtrees containing pointer-backed renderables (they
 * cannot persist -- same rule as scenes, but create fails loudly
 * instead of silently dropping: prefab bytes must round-trip). */
static led_prefab_node *led_prefab_collect(
    led_session *s, const le_object *root, uint32_t *out_n,
    le_object **out_members) {
    le_world *w = s->edit_world;
    le_engine *e = s->engine;
    uint32_t live = le_world_get_object_count(w);
    le_object *all = NULL;
    le_object *mem = NULL;
    led_prefab_node *nodes = NULL;
    uint32_t nmem = 0;
    uint32_t q;

    *out_n = 0;
    if (out_members != NULL) {
        *out_members = NULL;
    }
    if (live == 0) {
        return NULL;
    }
    if (!le_object_is_alive(w, root)) {
        return NULL;
    }
    all = (le_object *)malloc(live * sizeof(*all));
    mem = (le_object *)malloc(
        (live > 0 ? live : 1) * sizeof(*mem));
    if (all == NULL || mem == NULL) {
        free(all);
        free(mem);
        return NULL;
    }
    {
        uint32_t got = le_world_get_all_objects(w, all, live);

        for (q = 0; q < got; q++) {
            if (led_prefab_member(w, &all[q], root, got + 1u)) {
                mem[nmem++] = all[q];
            }
        }
    }
    free(all);
    if (nmem == 0) {
        free(mem);
        return NULL;
    }
    nodes = (led_prefab_node *)calloc(nmem, sizeof(*nodes));
    if (nodes == NULL) {
        free(mem);
        return NULL;
    }
    for (q = 0; q < nmem; q++) {
        led_prefab_node *sn = &nodes[q];
        const le_object *cur = &mem[q];
        const char *nm = le_object_get_name(w, cur);

        memset(sn, 0, sizeof(*sn));
        le_scene_object_id_make(e, &sn->local);
        strncpy(sn->name, nm != NULL ? nm : "",
                sizeof(sn->name) - 1);
        sn->enabled = le_object_is_enabled(w, cur);
        le_object_get_position(w, cur, sn->position);
        le_object_get_rotation(w, cur, sn->rotation);
        le_object_get_scale(w, cur, sn->scale);
        if (le_object_has_component(w, cur,
                                    LE_COMPONENT_RENDERABLE)) {
            le_renderable_desc probe;

            memset(&probe, 0, sizeof(probe));
            if (le_object_get_renderable(w, cur, &probe)) {
                /* Pointer-backed renderables have no persistent
                 * form: refuse loudly (never silent data loss).
                 * The caller frees nodes/members on NULL. */
                free(nodes);
                free(mem);
                *out_n = 0;
                if (out_members != NULL) {
                    *out_members = NULL;
                }
                return NULL;
            } else {
                le_asset_renderable_desc d;

                if (le_object_get_asset_renderable(w, cur, &d)) {
                    le_asset_get_id(e, &d.mesh, &sn->mesh_id);
                    le_asset_get_id(e, &d.material,
                                    &sn->material_id);
                    sn->has_asset_renderable = 1;
                    sn->casts_shadow = d.casts_shadow;
                    sn->receives_shadow = d.receives_shadow;
                    sn->visible = d.visible;
                }
            }
        }
        if (le_object_get_camera(w, cur, &sn->camera)) {
            sn->has_camera = 1;
        }
        if (le_object_get_light(w, cur, &sn->light)) {
            sn->has_light = 1;
        }
        if (le_object_has_component(w, cur,
                                    LE_COMPONENT_SCRIPT)) {
            le_asset sa = LE_ASSET_INVALID;

            if (le_object_get_script(w, cur, &sa)) {
                le_asset_get_id(e, &sa, &sn->script_id);
                sn->has_script = 1;
                {
                    le_script_property lp[16];
                    uint32_t sc = 0;
                    uint32_t k;

                    if (le_script_list_properties(w, cur, lp, 16,
                                                  &sc)) {
                        if (sc > 16) {
                            sc = 16;
                        }
                        for (k = 0; k < sc; k++) {
                            le_script_get_property(
                                w, cur, lp[k].name,
                                &sn->script_props[k]);
                        }
                        sn->script_prop_count = sc;
                    }
                }
            }
        }
        if (le_object_get_rigid_body(w, cur, &sn->body)) {
            sn->has_body = 1;
        }
        if (le_object_get_collider(w, cur, &sn->collider)) {
            sn->has_collider = 1;
        }
        {
            le_animator_desc ad;

            memset(&ad, 0, sizeof(ad));
            if (le_object_get_animator(w, cur, &ad)) {
                sn->has_animator = 1;
                if (le_asset_is_valid(&ad.skeleton)) {
                    le_asset_get_id(e, &ad.skeleton,
                                    &sn->skeleton_id);
                }
                if (le_asset_is_valid(&ad.clip)) {
                    le_asset_get_id(e, &ad.clip, &sn->clip_id);
                }
                sn->animator_autoplay = ad.autoplay;
                sn->animator_loop = (int)ad.loop_mode;
                sn->animator_speed = ad.speed;
                sn->animator_start_time = ad.start_time;
            }
        }
        if (le_object_get_character(w, cur, &sn->character)) {
            sn->has_character = 1;
        }
    }
    /* Parent links among members (O(n^2), n = subtree). The
     * selected root has no payload parent (payload roots attach on
     * instantiate). */
    for (q = 0; q < nmem; q++) {
        le_object p = LE_OBJECT_INVALID;
        uint32_t r2;

        if (!le_object_get_parent(w, &mem[q], &p)) {
            continue;
        }
        for (r2 = 0; r2 < nmem; r2++) {
            if (mem[r2].index == p.index &&
                mem[r2].generation == p.generation &&
                mem[r2].world_tag == p.world_tag) {
                nodes[q].parent_local = nodes[r2].local;
                nodes[q].has_parent = 1;
                break;
            }
        }
    }
    *out_n = nmem;
    if (out_members != NULL) {
        *out_members = mem;
    } else {
        free(mem);
    }
    return nodes;
}

/* ---- canonical writer (mirrors serialize.c discipline) ---- */

static void led_pf_fmt_float(char out[32], float v) {
    if (!(v == v)) {
        snprintf(out, 32, "0");
        return;
    }
    snprintf(out, 32, "%.9g", (double)v);
}

static void led_pf_hex64(uint64_t v, char out[17]) {
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        out[15 - i] = digits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

static void led_pf_id_str(const le_scene_object_id *id,
                          char out[33]) {
    char hi[17];
    char lo[17];

    memset(out, '0', 32);
    out[32] = '\0';
    if (id == NULL) {
        return;
    }
    led_pf_hex64(id->hi, hi);
    led_pf_hex64(id->lo, lo);
    memcpy(out, hi, 16);
    memcpy(out + 16, lo, 16);
}

static void led_pf_asset_str(const le_asset_id *id, char out[33]) {
    char hi[17];
    char lo[17];

    memset(out, '0', 32);
    out[32] = '\0';
    if (id == NULL) {
        return;
    }
    led_pf_hex64(id->hi, hi);
    led_pf_hex64(id->lo, lo);
    memcpy(out, hi, 16);
    memcpy(out + 16, lo, 16);
}

typedef struct led_pf_buf {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} led_pf_buf;

static void led_pf_emit(led_pf_buf *b, const char *line) {
    size_t n;

    if (b->oom) {
        return;
    }
    n = strlen(line);
    if (b->len + n + 1 > b->cap) {
        size_t grown = (b->cap == 0) ? 4096 : b->cap * 2u;

        while (grown < b->len + n + 1) {
            grown *= 2u;
            if (grown > 64u * 1024u * 1024u) {
                b->oom = 1;
                return;
            }
        }
        {
            char *fresh = (char *)realloc(b->data, grown);

            if (fresh == NULL) {
                b->oom = 1;
                return;
            }
            b->data = fresh;
            b->cap = grown;
        }
    }
    memcpy(b->data + b->len, line, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void led_pf_escape(led_pf_buf *b, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    char tmp[8];

    if (s == NULL) {
        return;
    }
    while (*p != '\0') {
        if (*p == '"') {
            led_pf_emit(b, "\\\"");
        } else if (*p == '\\') {
            led_pf_emit(b, "\\\\");
        } else if (*p == '\n') {
            led_pf_emit(b, "\\n");
        } else if (*p == '\t') {
            led_pf_emit(b, "\\t");
        } else if (*p == '\r') {
            led_pf_emit(b, "\\r");
        } else if (*p < 0x20 || *p == 0x7F) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            led_pf_emit(b, tmp);
        } else {
            char one[2];

            one[0] = (char)*p;
            one[1] = '\0';
            led_pf_emit(b, one);
        }
        p++;
    }
}

/* Emit one prefab node (scene-vocabulary lines + end).
 * sprop_asset_hexes[i] parallels sn->script_props (hex per
 * asset-valued prop, resolved at create time when the engine is
 * available; NULL when the node has no asset sprops). */
static void led_pf_emit_node(led_pf_buf *b,
                             const led_prefab_node *sn,
                             char (*sprop_asset_hexes)[33]) {
    char id[33];
    char line[2048];
    char f[8][32];
    int k;

    led_pf_id_str(&sn->local, id);
    snprintf(line, sizeof(line), "object %s\n", id);
    led_pf_emit(b, line);
    if (sn->name[0] != '\0') {
        led_pf_emit(b, "name \"");
        led_pf_escape(b, sn->name);
        led_pf_emit(b, "\"\n");
    }
    snprintf(line, sizeof(line), "enabled %d\n",
             sn->enabled ? 1 : 0);
    led_pf_emit(b, line);
    if (sn->has_parent) {
        char ph[33];

        led_pf_id_str(&sn->parent_local, ph);
        snprintf(line, sizeof(line), "parent %s\n", ph);
    } else {
        snprintf(line, sizeof(line), "parent nil\n");
    }
    led_pf_emit(b, line);
    for (k = 0; k < 3; k++) {
        led_pf_fmt_float(f[k], sn->position[k]);
    }
    snprintf(line, sizeof(line), "position %s %s %s\n", f[0], f[1],
             f[2]);
    led_pf_emit(b, line);
    for (k = 0; k < 4; k++) {
        led_pf_fmt_float(f[k], sn->rotation[k]);
    }
    snprintf(line, sizeof(line), "rotation %s %s %s %s\n", f[0],
             f[1], f[2], f[3]);
    led_pf_emit(b, line);
    for (k = 0; k < 3; k++) {
        led_pf_fmt_float(f[k], sn->scale[k]);
    }
    snprintf(line, sizeof(line), "scale %s %s %s\n", f[0], f[1],
             f[2]);
    led_pf_emit(b, line);
    if (sn->has_asset_renderable) {
        char mh[33];
        char th[33];

        led_pf_asset_str(&sn->mesh_id, mh);
        led_pf_asset_str(&sn->material_id, th);
        snprintf(line, sizeof(line),
                 "renderable %s %s %d %d %d\n", mh, th,
                 sn->casts_shadow ? 1 : 0,
                 sn->receives_shadow ? 1 : 0,
                 sn->visible ? 1 : 0);
        led_pf_emit(b, line);
    }
    if (sn->has_camera) {
        char lens[32];
        char aspect[32];
        char near[32];
        char far[32];

        led_pf_fmt_float(lens,
                         sn->camera.projection ==
                                 LE_PROJECTION_PERSPECTIVE
                             ? sn->camera.fov_y_rad
                             : sn->camera.ortho_height);
        led_pf_fmt_float(aspect, sn->camera.aspect);
        led_pf_fmt_float(near, sn->camera.near_plane);
        led_pf_fmt_float(far, sn->camera.far_plane);
        snprintf(line, sizeof(line), "camera %s %s %s %s %s\n",
                 sn->camera.projection == LE_PROJECTION_PERSPECTIVE
                     ? "persp"
                     : "ortho",
                 lens, aspect, near, far);
        led_pf_emit(b, line);
    }
    if (sn->has_light) {
        char c[3][32];
        char in[32];
        char rg[32];
        char si[32];
        char so[32];

        for (k = 0; k < 3; k++) {
            led_pf_fmt_float(c[k], sn->light.color[k]);
        }
        led_pf_fmt_float(in, sn->light.intensity);
        led_pf_fmt_float(rg, sn->light.range);
        led_pf_fmt_float(si, sn->light.spot_inner);
        led_pf_fmt_float(so, sn->light.spot_outer);
        snprintf(line, sizeof(line),
                 "light %s %s %s %s %s %s %s %s\n",
                 sn->light.type == LE_LIGHT_DIRECTIONAL
                     ? "dir"
                     : (sn->light.type == LE_LIGHT_POINT ? "point"
                                                         : "spot"),
                 c[0], c[1], c[2], in, rg, si, so);
        led_pf_emit(b, line);
        /* Shadow state rides verbatim (same 8-token canonical form
         * as scenes: `shadow 0` or `shadow 1 <res> <bias...>`);
         * omitting it would silently reset shadowed lights. */
        if (sn->light.shadow.enabled) {
            char db[32];
            char nb[32];
            char snp[32];
            char sf[32];
            char sd[32];

            led_pf_fmt_float(db, sn->light.shadow.depth_bias);
            led_pf_fmt_float(nb,
                             sn->light.shadow.normal_bias);
            led_pf_fmt_float(snp,
                             sn->light.shadow.near_plane);
            led_pf_fmt_float(sf, sn->light.shadow.far_plane);
            led_pf_fmt_float(sd,
                             sn->light.shadow.shadow_distance);
            snprintf(line, sizeof(line),
                     "shadow 1 %u %s %s %s %s %s\n",
                     sn->light.shadow.resolution, db, nb, snp,
                     sf, sd);
        } else {
            snprintf(line, sizeof(line), "shadow 0\n");
        }
        led_pf_emit(b, line);
    }
    if (sn->has_script) {
        char sh[33];

        led_pf_asset_str(&sn->script_id, sh);
        snprintf(line, sizeof(line), "script %s\n", sh);
        led_pf_emit(b, line);
        for (k = 0; k < (int)sn->script_prop_count; k++) {
            const le_script_property *sp = &sn->script_props[k];
            char nm[128];
            size_t ni = 0;

            while (sp->name[ni] != '\0' && ni + 1 < sizeof(nm)) {
                nm[ni] = sp->name[ni];
                ni++;
            }
            nm[ni] = '\0';
            switch (sp->type) {
            case LE_SCRIPT_PROP_BOOL:
                snprintf(line, sizeof(line), "sprop bool %s %d\n",
                         nm, sp->boolean ? 1 : 0);
                led_pf_emit(b, line);
                break;
            case LE_SCRIPT_PROP_INT:
                snprintf(line, sizeof(line), "sprop int %s %lld\n",
                         nm, (long long)sp->integer);
                led_pf_emit(b, line);
                break;
            case LE_SCRIPT_PROP_NUMBER: {
                char nb[32];

                snprintf(nb, sizeof(nb), "%.17g", sp->number);
                snprintf(line, sizeof(line),
                         "sprop number %s %s\n", nm, nb);
                led_pf_emit(b, line);
                break;
            }
            case LE_SCRIPT_PROP_STRING:
                snprintf(line, sizeof(line), "sprop string %s \"",
                         nm);
                led_pf_emit(b, line);
                led_pf_escape(b, sp->string_value);
                led_pf_emit(b, "\"\n");
                break;
            case LE_SCRIPT_PROP_VEC3: {
                char x[32];
                char y[32];
                char z[32];

                led_pf_fmt_float(x, sp->vec3[0]);
                led_pf_fmt_float(y, sp->vec3[1]);
                led_pf_fmt_float(z, sp->vec3[2]);
                snprintf(line, sizeof(line),
                         "sprop vec3 %s %s %s %s\n", nm, x, y,
                         z);
                led_pf_emit(b, line);
                break;
            }
            case LE_SCRIPT_PROP_ASSET: {
                const char *hex =
                    (sprop_asset_hexes != NULL &&
                     (uint32_t)k < 16u)
                        ? sprop_asset_hexes[k]
                        : "00000000000000000000000000000000";

                snprintf(line, sizeof(line),
                         "sprop asset %s %s\n", nm, hex);
                led_pf_emit(b, line);
                break;
            }
            default:
                break;
            }
        }
    }
    if (sn->has_body) {
        char m[32];
        char ld[32];
        char ad[32];
        char gs[32];
        char lv[3][32];
        char av[3][32];

        led_pf_fmt_float(m, sn->body.mass);
        led_pf_fmt_float(ld, sn->body.linear_damping);
        led_pf_fmt_float(ad, sn->body.angular_damping);
        led_pf_fmt_float(gs, sn->body.gravity_scale);
        for (k = 0; k < 3; k++) {
            led_pf_fmt_float(lv[k], sn->body.linear_velocity[k]);
            led_pf_fmt_float(av[k], sn->body.angular_velocity[k]);
        }
        snprintf(line, sizeof(line),
                 "rigid_body %s %s %s %s %s %s %s %s %s %s %s %s\n",
                 sn->body.type == LE_BODY_STATIC
                     ? "static"
                     : (sn->body.type == LE_BODY_DYNAMIC
                            ? "dynamic"
                            : "kinematic"),
                 m, ld, ad, gs, lv[0], lv[1], lv[2], av[0], av[1],
                 av[2]);
        led_pf_emit(b, line);
    }
    if (sn->has_collider) {
        char off[3][32];
        char q[4][32];
        char fr[32];
        char re[32];

        for (k = 0; k < 3; k++) {
            led_pf_fmt_float(off[k], sn->collider.offset[k]);
        }
        for (k = 0; k < 4; k++) {
            led_pf_fmt_float(q[k], sn->collider.orientation[k]);
        }
        led_pf_fmt_float(fr, sn->collider.friction);
        led_pf_fmt_float(re, sn->collider.restitution);
        if (sn->collider.shape == LE_COLLIDER_SPHERE) {
            char r[32];

            led_pf_fmt_float(r, sn->collider.radius);
            snprintf(line, sizeof(line),
                     "collider sphere %s %s %s %s %s %s %s %s %d "
                     "%u %u %s %s\n",
                     r, off[0], off[1], off[2], q[0], q[1], q[2],
                     q[3], sn->collider.is_trigger ? 1 : 0,
                     sn->collider.layer, sn->collider.mask, fr,
                     re);
        } else if (sn->collider.shape == LE_COLLIDER_CAPSULE) {
            char r[32];
            char hh[32];

            led_pf_fmt_float(r, sn->collider.capsule_radius);
            led_pf_fmt_float(hh,
                             sn->collider.capsule_half_height);
            snprintf(line, sizeof(line),
                     "collider capsule %s %s %s %s %s %s %s %s %s "
                     "%d %u %u %s %s\n",
                     r, hh, off[0], off[1], off[2], q[0], q[1],
                     q[2], q[3],
                     sn->collider.is_trigger ? 1 : 0,
                     sn->collider.layer, sn->collider.mask, fr,
                     re);
        } else {
            char he[3][32];

            for (k = 0; k < 3; k++) {
                led_pf_fmt_float(he[k],
                                 sn->collider.half_extents[k]);
            }
            snprintf(line, sizeof(line),
                     "collider box %s %s %s %s %s %s %s %s %s %s "
                     "%d %u %u %s %s\n",
                     he[0], he[1], he[2], off[0], off[1], off[2],
                     q[0], q[1], q[2], q[3],
                     sn->collider.is_trigger ? 1 : 0,
                     sn->collider.layer, sn->collider.mask, fr,
                     re);
        }
        led_pf_emit(b, line);
    }
    if (sn->has_animator) {
        char sk[33];
        char cl[33];
        char sp[32];
        char st[32];

        led_pf_asset_str(&sn->skeleton_id, sk);
        led_pf_asset_str(&sn->clip_id, cl);
        if (le_asset_id_is_nil(&sn->skeleton_id)) {
            memcpy(sk, "nil", 4);
        }
        if (le_asset_id_is_nil(&sn->clip_id)) {
            memcpy(cl, "nil", 4);
        }
        led_pf_fmt_float(sp, sn->animator_speed);
        led_pf_fmt_float(st, sn->animator_start_time);
        snprintf(line, sizeof(line), "animator %s %s %d %s %s %s\n",
                 sk, cl, sn->animator_autoplay ? 1 : 0,
                 sn->animator_loop == 1
                     ? "loop"
                     : (sn->animator_loop == 2 ? "pingpong"
                                               : "once"),
                 sp, st);
        led_pf_emit(b, line);
    }
    if (sn->has_character) {
        char f[11][32];
        char push[32];

        led_pf_fmt_float(f[0], sn->character.radius);
        led_pf_fmt_float(f[1], sn->character.height);
        led_pf_fmt_float(f[2], sn->character.up[0]);
        led_pf_fmt_float(f[3], sn->character.up[1]);
        led_pf_fmt_float(f[4], sn->character.up[2]);
        led_pf_fmt_float(f[5], sn->character.skin_width);
        led_pf_fmt_float(
            f[6], sn->character.max_slope_angle * 57.29578f);
        led_pf_fmt_float(f[7], sn->character.step_height);
        led_pf_fmt_float(f[8], sn->character.gravity);
        led_pf_fmt_float(f[9], sn->character.terminal_velocity);
        led_pf_fmt_float(f[10], sn->character.snap_distance);
        led_pf_fmt_float(push, sn->character.push_strength);
        snprintf(line, sizeof(line),
                 "character %s %s %s %s %s %s %s %s %s %s %s "
                 "%s %u %u\n",
                 f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                 f[8], f[9], f[10], push, sn->character.layer,
                 sn->character.mask);
        led_pf_emit(b, line);
    }
    led_pf_emit(b, "end\n");
}

/* ---- create: subtree -> canonical text -> file + sidecar + DB ---- */

led_result led_prefab_create(led_session *session,
                             const le_object *root,
                             const char *rel_path) {
    char norm[1024];
    char abs[2048];
    led_prefab_node *nodes = NULL;
    uint32_t n = 0;
    le_object *members = NULL;

    if (session == NULL || root == NULL || rel_path == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    if (!led_project_normalize(rel_path, norm)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    {
        size_t nl = strlen(norm);

        /* ".luprefab" is 9 chars (dot included). */
        if (nl < 10 || strcmp(norm + nl - 9, ".luprefab") != 0) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
    }
    if (!led_project_resolve(session, norm, abs)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!le_object_is_alive(session->edit_world, root)) {
        return LED_ERROR_STALE_HANDLE;
    }
    nodes = led_prefab_collect(session, root, &n, &members);
    if (nodes == NULL || n == 0) {
        free(nodes);
        free(members);
        /* Collect fails NULL on OOM AND on
         * unpersistable content (pointer-backed renderable)
         * AND on stale root (pre-checked alive above, but the
         * world may hold zero objects): report OOM only when
         * the subtree is non-trivially collectible. The
         * unpersistable case is a VALIDATION failure (loud,
         * not silent) — detect via a live-member probe. */
        {
            uint32_t live = le_world_get_object_count(
                session->edit_world);

            if (live > 0) {
                return LED_ERROR_VALIDATION;
            }
        }
        return LED_ERROR_OUT_OF_MEMORY;
    }
    /* Refuse overwriting an existing prefab source (explicit
     * delete first -- no silent overwrite). */
    {
        FILE *probe = fopen(abs, "r");

        if (probe != NULL) {
            fclose(probe);
            free(nodes);
            free(members);
            return LED_ERROR_VALIDATION;
        }
    }
    /* Resolve asset-valued sprop hexes NOW (engine available;
     * per-node hex rows: sprop k of node i lives at
     * hexes[i*16+k]; uniform rows (n*16) capped by the engine's
     * LE_SCRIPT_MAX_PROPS=16 invariant). */
    {
        char (*hexes)[33] = NULL;
        uint32_t i;
        int failed = 0;

        /* Per-node hex rows (uniform, capped by the prop-count
         * invariant: script_prop_count <= 16 enforced at collect
         * time -- see the clamp after le_script_list_properties).
         * calloc: (n>0) guaranteed here (empty collect returns
         * NULL above). */
        hexes = (char (*)[33])calloc(n * 16u, 33);
        if (hexes == NULL) {
            free(nodes);
            free(members);
            return LED_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < n; i++) {
            uint32_t k;

            for (k = 0; k < nodes[i].script_prop_count; k++) {
                if (nodes[i].script_props[k].type ==
                    LE_SCRIPT_PROP_ASSET) {
                    le_asset_id aid;

                    memset(&aid, 0, sizeof(aid));
                    le_asset_get_id(
                        session->engine,
                        &nodes[i].script_props[k].asset, &aid);
                    if (le_asset_id_is_nil(&aid)) {
                        failed = 1;
                        break;
                    }
                    led_pf_asset_str(&aid, hexes[i * 16u + k]);
                }
            }
            if (failed) {
                break;
            }
        }
        if (!failed) {
            /* Emit canonical text (ID-sorted for determinism). */
            uint32_t *order = NULL;
            uint32_t j;

            order = (uint32_t *)malloc(n * sizeof(*order));
            if (order == NULL) {
                free(hexes);
                free(nodes);
                free(members);
                return LED_ERROR_OUT_OF_MEMORY;
            }
            for (j = 0; j < n; j++) {
                order[j] = j;
            }
            {
                /* Canonical order = ID-sorted records (same rule as
                 * scenes: ascending-ID bytes). O(n^2) insertion --
                 * subtree collects are editor-scale (hundreds). */
                uint32_t a;
                uint32_t b2;

                for (a = 0; a < n; a++) {
                    for (b2 = a + 1u; b2 < n; b2++) {
                        const led_prefab_node *na =
                            &nodes[order[a]];
                        const led_prefab_node *nb =
                            &nodes[order[b2]];
                        int cmp =
                            (na->local.hi > nb->local.hi)
                                ? 1
                                : ((na->local.hi < nb->local.hi)
                                       ? -1
                                       : ((na->local.lo >
                                           nb->local.lo)
                                              ? 1
                                              : ((na->local.lo <
                                                  nb->local.lo)
                                                     ? -1
                                                     : 0)));

                        if (cmp > 0) {
                            uint32_t t = order[a];

                            order[a] = order[b2];
                            order[b2] = t;
                        } else if (cmp == 0) {
                            failed = 1; /* duplicate local ID */
                            break;
                        }
                    }
                    if (failed) {
                        break;
                    }
                }
            }
            if (!failed) {
                led_pf_buf b;
                led_project_asset_id prefab_uuid;
                char prefab_hex[33];

                memset(&b, 0, sizeof(b));
                led_project_id_mint(&prefab_uuid);
                {
                    char tmp[33];

                    /* Reuse the hex writer on the raw halves. */
                    le_scene_object_id tmpid;

                    tmpid.hi = prefab_uuid.hi;
                    tmpid.lo = prefab_uuid.lo;
                    led_pf_id_str(&tmpid, tmp);
                    memcpy(prefab_hex, tmp, 33);
                }
                {
                    char line[128];

                    /* Header, then the prefab identity line, then
                     * ID-sorted object records (canonical order),
                     * then the root line. */
                    led_pf_emit(&b, "LUMA_PREFAB 1\n");
                    snprintf(line, sizeof(line), "prefab %s\n",
                             prefab_hex);
                    led_pf_emit(&b, line);
                    for (j = 0; j < n && !b.oom; j++) {
                        led_pf_emit_node(&b, &nodes[order[j]],
                                         &hexes[order[j] * 16u]);
                    }
                    /* prefab_root = the selected root's local ID
                     * (the member matching `root` by full handle
                     * identity -- collect guarantees membership,
                     * so the loop below always terminates with
                     * a root line). */
                    {
                        uint32_t r3;

                        for (r3 = 0; r3 < n; r3++) {
                            if (members[r3].index ==
                                    root->index &&
                                members[r3].generation ==
                                    root->generation &&
                                members[r3].world_tag ==
                                    root->world_tag) {
                                char rh[33];
                                char rl[128];

                                led_pf_id_str(
                                    &nodes[r3].local, rh);
                                snprintf(rl, sizeof(rl),
                                         "prefab_root %s\n", rh);
                                led_pf_emit(&b, rl);
                                break;
                            }
                        }
                    }
                }
                if (!b.oom && b.data != NULL) {
                    /* Binary mode (like le_scene_save_file):
                     * canonical bytes must not gain \r on
                     * Windows — load tolerates \r, but
                     * byte-stability across platforms needs
                     * exact LF. */
                    FILE *f = fopen(abs, "wb");

                    if (f == NULL) {
                        free(b.data);
                        free(order);
                        free(hexes);
                        free(nodes);
                        free(members);
                        return LED_ERROR_IO;
                    }
                    if (fwrite(b.data, 1, b.len, f) != b.len ||
                        fclose(f) != 0) {
                        remove(abs);
                        free(b.data);
                        free(order);
                        free(hexes);
                        free(nodes);
                        free(members);
                        return LED_ERROR_IO;
                    }
                    free(b.data);
                    /* Register the record directly (sidecar first
                     * so discovery adopts our UUID; no rescan --
                     * the DB write below is the adoption). */
                    {
                        led_project *p = session->project;

                        /* Force-adopt: create the record directly
                         * (a scan would mint a fresh UUID; we keep
                         * the one already stored in the `prefab`
                         * payload line). */
                        if (p->record_count >= LED_PROJECT_MAX_RECORDS) {
                            free(order);
                            free(hexes);
                            free(nodes);
                            free(members);
                            remove(abs);
                            return LED_ERROR_OVERFLOW;
                        }
                        if (p->record_count >= p->record_cap) {
                            uint32_t grown =
                                (p->record_cap == 0)
                                    ? 256u
                                    : p->record_cap * 2u;
                            led_db_record *fresh =
                                (led_db_record *)realloc(
                                    p->records,
                                    grown * sizeof(*fresh));

                            if (fresh == NULL) {
                                free(order);
                                free(hexes);
                                free(nodes);
                                free(members);
                                remove(abs);
                                return LED_ERROR_OUT_OF_MEMORY;
                            }
                            p->records = fresh;
                            p->record_cap = grown;
                        }
                        {
                            led_db_record *r =
                                &p->records[p->record_count];

                            memset(r, 0, sizeof(*r));
                            r->runtime_asset =
                                LE_ASSET_INVALID;
                            strncpy(r->source_path, norm,
                                    sizeof(r->source_path) -
                                        1);
                            r->type =
                                LED_PROJECT_ASSET_PREFAB;
                            r->status =
                                LED_IMPORT_UNIMPORTED;
                            r->id = prefab_uuid;
                            r->has_sidecar = 1;
                            strncpy(r->importer,
                                    led_importer_id_for(
                                        LED_PROJECT_ASSET_PREFAB),
                                    sizeof(r->importer) - 1);
                            r->importer_version =
                                led_importer_version_for(
                                    LED_PROJECT_ASSET_PREFAB);
                            r->settings_digest =
                                led_import_settings_digest(
                                    LED_PROJECT_ASSET_PREFAB);
                            /* Fingerprint the bytes we wrote. */
                            {
                                /* Small stack buffer (4KB — the
                                 * 64KB stack buffers elsewhere
                                 * were shrunk for the same
                                 * reason). */
                                unsigned char fbuf[4096];
                                FILE *ff = NULL;
                                size_t nn = 0;
                                uint64_t hh =
                                    14695981039346656037ull;
                                uint64_t sz = 0;

                                ff = fopen(abs, "rb");
                                if (ff != NULL) {
                                    while (
                                        (nn = fread(
                                             fbuf, 1,
                                             sizeof(fbuf),
                                             ff)) > 0) {
                                        size_t zi;

                                        for (zi = 0;
                                             zi < nn; zi++) {
                                            hh ^=
                                                (uint64_t)
                                                    fbuf[zi];
                                            hh *=
                                                1099511628211ull;
                                        }
                                        sz += (uint64_t)nn;
                                    }
                                    fclose(ff);
                                }
                                r->fp_size = sz;
                                r->fp_hash = hh;
                            }
                            p->record_count++;
                            /* Dependency extraction: asset IDs
                             * referenced by the nodes bridge to
                             * project records via the stored
                             * runtime le_asset_id (deduped
                             * edges; self-edges skipped). */
                            {
                                uint32_t a2;

                                for (a2 = 0; a2 < n; a2++) {
                                    le_asset_id aids[6];
                                    int naids = 0;
                                    int t2;

                                    if (nodes[a2]
                                            .has_asset_renderable) {
                                        aids[naids++] =
                                            nodes[a2].mesh_id;
                                        aids[naids++] =
                                            nodes[a2]
                                                .material_id;
                                    }
                                    if (nodes[a2]
                                            .has_script) {
                                        aids[naids++] =
                                            nodes[a2]
                                                .script_id;
                                    }
                                    if (nodes[a2]
                                            .has_animator) {
                                        if (!le_asset_id_is_nil(
                                                &nodes[a2]
                                                     .skeleton_id)) {
                                            aids[naids++] =
                                                nodes[a2]
                                                    .skeleton_id;
                                        }
                                        if (!le_asset_id_is_nil(
                                                &nodes[a2]
                                                     .clip_id)) {
                                            aids[naids++] =
                                                nodes[a2]
                                                    .clip_id;
                                        }
                                    }
                                    /* Asset-valued script props
                                     * (first one; the rest
                                     * re-resolve at load through
                                     * the same registry bridge). */
                                    {
                                        uint32_t k2;

                                        for (
                                            k2 = 0;
                                            k2 <
                                            nodes[a2]
                                                .script_prop_count;
                                            k2++) {
                                            if (nodes[a2]
                                                    .script_props
                                                        [k2]
                                                    .type ==
                                                LE_SCRIPT_PROP_ASSET) {
                                                le_asset_id aid;

                                                memset(
                                                    &aid, 0,
                                                    sizeof(
                                                        aid));
                                                le_asset_get_id(
                                                    session
                                                        ->engine,
                                                    &nodes[a2]
                                                         .script_props
                                                             [k2]
                                                         .asset,
                                                    &aid);
                                                if (!le_asset_id_is_nil(
                                                        &aid)) {
                                                    aids[naids++] =
                                                        aid;
                                                }
                                                break;
                                            }
                                        }
                                    }
                                    for (t2 = 0; t2 < naids;
                                         t2++) {
                                        uint32_t r5;

                                        for (
                                            r5 = 0;
                                            r5 <
                                            p->record_count;
                                            r5++) {
                                            uint32_t d2;
                                            int have = 0;

                                            if (!p->records[r5]
                                                     .has_runtime_id ||
                                                !le_asset_id_equal(
                                                    &p->records
                                                         [r5]
                                                             .runtime_id,
                                                    &aids[t2])) {
                                                continue;
                                            }
                                            if (led_project_id_equal(
                                                    &p->records
                                                         [r5]
                                                             .id,
                                                    &r->id)) {
                                                break; /* self */
                                            }
                                            for (
                                                d2 = 0;
                                                d2 <
                                                r->dep_count;
                                                d2++) {
                                                if (led_project_id_equal(
                                                        &r->deps
                                                            [d2],
                                                        &p->records
                                                             [r5]
                                                                 .id)) {
                                                    have =
                                                        1;
                                                    break;
                                                }
                                            }
                                            if (!have) {
                                                uint32_t grown =
                                                    (r->dep_cap ==
                                                     0)
                                                        ? 4u
                                                        : r->dep_cap *
                                                          2u;
                                                led_project_asset_id *
                                                    fresh =
                                                        (led_project_asset_id *)
                                                            realloc(
                                                                r->deps,
                                                                grown *
                                                                    sizeof(*fresh));

                                                if (fresh ==
                                                    NULL) {
                                                    break;
                                                }
                                                r->deps =
                                                    fresh;
                                                r->dep_cap =
                                                    grown;
                                                r->deps[r->dep_count++] =
                                                    p->records
                                                        [r5]
                                                            .id;
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                            /* The record is complete except its
                             * sort position: write the sidecar
                             * (canonical led_sidecar_write_pub),
                             * sort, reindex, refresh the view.
                             * No rescan (identity already
                             * adopted -- a scan would only
                             * re-read what we just wrote). */
                            led_sidecar_write_pub(p, r);
                            if (p->record_count > 1) {
                                qsort(p->records,
                                      p->record_count,
                                      sizeof(*p->records),
                                      led_record_cmp_pub);
                            }
                            led_db_rebuild(p);
                            led_browser_refresh(session);
                            free(order);
                            free(hexes);
                            free(nodes);
                            free(members);
                            return LED_SUCCESS;
                        }
                    }
                }
                free(b.data);
                free(order);
                free(hexes);
                free(nodes);
                free(members);
                return LED_ERROR_OUT_OF_MEMORY;
            }
        }
        free(hexes);
        free(nodes);
        free(members);
        return LED_ERROR_VALIDATION;
    }
}

/* ------------------------------------------------------------------
 * load: file -> validate -> LE_ASSET_PREFAB registry slot
 * (transactional; world untouched on failure).
 *
 * Validation reuses the SCENE parser, not a second prefab parser:
 * the payload is translated to scene vocabulary in memory
 * (`LUMA_PREFAB 1` -> `LUMA_SCENE 1`, `prefab <uuid>` dropped, and
 * `parent nil` roots kept as scene roots — the scene commit path
 * only needs records + intra-payload parent links, which are
 * identical), then le_scene_load_text enforces the SAME
 * fail-closed rules as scenes (shape, IDs, components, TRS,
 * hierarchy, asset-ID resolution). Prefab-shape rules the scene
 * parser cannot выразить (exactly-one identity line first, exactly
 * one root line naming a known ID, no second `prefab` line, no
 * `asset` table) are checked here, line by line, BEFORE the
 * translation is parsed.
 * ------------------------------------------------------------------ */

/* Hex helpers shared with the writer. */
static int led_pf_hexval(char c, unsigned *out) {
    if (c >= '0' && c <= '9') {
        *out = (unsigned)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (unsigned)(c - 'a') + 10u;
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (unsigned)(c - 'A') + 10u;
        return 1;
    }
    return 0;
}

static int led_pf_parse_hex32(const char *s, uint64_t *hi,
                              uint64_t *lo) {
    uint64_t h = 0;
    uint64_t l = 0;
    int i;

    if (s == NULL || strlen(s) != 32) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        unsigned v = 0;

        if (!led_pf_hexval(s[i], &v)) {
            return 0;
        }
        if (i < 16) {
            h = (h << 4) | (uint64_t)v;
        } else {
            l = (l << 4) | (uint64_t)v;
        }
    }
    if (hi != NULL) {
        *hi = h;
    }
    if (lo != NULL) {
        *lo = l;
    }
    return 1;
}

/* Read a whole file (bounded 64MB). Returns 1 + NUL-terminated
 * bytes; 0 on absent/unreadable/overlarge/empty. */
static int led_pf_read_file(const char *abs, char **out_text,
                            size_t *out_size) {
    FILE *f = NULL;
    long n = 0;
    char *buf = NULL;

    *out_text = NULL;
    *out_size = 0;
    f = fopen(abs, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n <= 0 ||
        (uint64_t)n > LED_PREFAB_MAX_TEXT) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    *out_size = (size_t)n;
    return 1;
}

/* Load-time validation + scene translation.
 * On success: *out_scene_text holds a NUL-terminated
 * `LUMA_SCENE 1` translation (caller frees), *out_size its byte
 * count, *out_prefab the prefab UUID, *out_root the root local ID.
 * Failure returns an led_result (PARSE for malformed, ENGINE for
 * impossible — none expected — OOM for allocation). */
static led_result led_pf_translate(const char *text, size_t size,
                                   char **out_scene_text,
                                   size_t *out_size,
                                   led_project_asset_id *out_prefab,
                                   le_scene_object_id *out_root) {
    char *copy = NULL;
    char *scene = NULL;
    size_t slen = 0;
    size_t scap = 0;
    int oom = 0;
    /* Shape state. */
    int lineno = 0;
    int saw_prefab = 0;
    int saw_root = 0;
    int in_object = 0;
    uint32_t object_count = 0;
    led_project_asset_id prefab_uuid;
    le_scene_object_id root_id;
    /* Local-ID table for the root cross-check (dup detect happens
     * in the scene parser; here we only need existence). */
    le_scene_object_id *locals = NULL;
    uint32_t nlocals = 0;
    uint32_t clocals = 0;

    memset(&prefab_uuid, 0, sizeof(prefab_uuid));
    memset(&root_id, 0, sizeof(root_id));
    *out_scene_text = NULL;
    *out_size = 0;
    if (text == NULL || size == 0 || size > LED_PREFAB_MAX_TEXT) {
        return LED_ERROR_PARSE;
    }
    copy = (char *)malloc(size + 1u);
    if (copy == NULL) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, size);
    copy[size] = '\0';
    /* Scene buffer: translation is SHORTER than the source (the
     * `prefab` line drops). Start at source size + header slack. */
    scap = size + 32u;
    scene = (char *)malloc(scap);
    if (scene == NULL) {
        free(copy);
        return LED_ERROR_OUT_OF_MEMORY;
    }
    {
        char *p = copy;

        while (1) {
            char *eol = strchr(p, '\n');
            char *ln;
            size_t llen;

            if (eol != NULL) {
                *eol = '\0';
                ln = p;
                p = eol + 1;
            } else {
                ln = p;
                p = p + strlen(p);
            }
            llen = strlen(ln);
            if (llen > 0 && ln[llen - 1] == '\r') {
                ln[--llen] = '\0';
            }
            /* Skip blanks/comments in LOCKSTEP (both payload and
             * translation skip them: the scene parser ignores
             * blanks/comments too, so line correspondence holds). */
            if (ln[0] == '\0' || ln[0] == '#') {
                if (*p == '\0') {
                    break;
                }
                continue;
            }
            lineno++;
            if (lineno == 1) {
                if (strcmp(ln, LED_PREFAB_MAGIC) != 0) {
                    free(copy);
                    free(scene);
                    free(locals);
                    return LED_ERROR_PARSE;
                }
                /* Translate the header. */
                {
                    const char *hdr = "LUMA_SCENE 1\n";
                    size_t hn = strlen(hdr);

                    if (hn + 1 > scap) {
                        oom = 1;
                        break;
                    }
                    memcpy(scene, hdr, hn + 1u);
                    slen = hn;
                }
                if (*p == '\0') {
                    break;
                }
                continue;
            }
            /* Tokenize the first word (structural dispatch only;
             * the scene parser re-validates every line fully). */
            {
                char word[32];
                size_t wi = 0;
                size_t qi = 0;

                while (ln[qi] == ' ' || ln[qi] == '\t') {
                    qi++;
                }
                while (ln[qi] != '\0' && ln[qi] != ' ' &&
                       ln[qi] != '\t' && wi + 1 < sizeof(word)) {
                    word[wi++] = ln[qi++];
                }
                word[wi] = '\0';
                if (strcmp(word, "prefab") == 0) {
                    /* Exactly one, and it must be line 2 (right
                     * after the header). */
                    if (saw_prefab || lineno != 2) {
                        free(copy);
                        free(scene);
                        free(locals);
                        return LED_ERROR_PARSE;
                    }
                    {
                        /* prefab <32hex>: skip word + blanks. */
                        const char *arg = ln + qi;

                        while (*arg == ' ' || *arg == '\t') {
                            arg++;
                        }
                        if (!led_pf_parse_hex32(
                                arg, &prefab_uuid.hi,
                                &prefab_uuid.lo) ||
                            (prefab_uuid.hi == 0 &&
                             prefab_uuid.lo == 0)) {
                            free(copy);
                            free(scene);
                            free(locals);
                            return LED_ERROR_PARSE;
                        }
                        /* Trailing garbage after the 32 hex digits
                         * is malformed (strict fixed-arity lines,
                         * same as the scene parser). */
                        {
                            const char *tail = arg + 32;

                            while (*tail == ' ' ||
                                   *tail == '\t') {
                                tail++;
                            }
                            if (*tail != '\0') {
                                free(copy);
                                free(scene);
                                free(locals);
                                return LED_ERROR_PARSE;
                            }
                        }
                    }
                    saw_prefab = 1;
                    /* Dropped from the translation (identity
                     * travels via out_prefab). */
                } else if (strcmp(word, "object") == 0) {
                    const char *arg = ln + qi;
                    uint64_t hi = 0;
                    uint64_t lo = 0;

                    if (!saw_prefab) {
                        free(copy);
                        free(scene);
                        free(locals);
                        return LED_ERROR_PARSE;
                    }
                    while (*arg == ' ' || *arg == '\t') {
                        arg++;
                    }
                    if (!led_pf_parse_hex32(arg, &hi, &lo) ||
                        (hi == 0 && lo == 0)) {
                        free(copy);
                        free(scene);
                        free(locals);
                        return LED_ERROR_PARSE;
                    }
                    {
                        const char *tail = arg + 32;

                        while (*tail == ' ' || *tail == '\t') {
                            tail++;
                        }
                        if (*tail != '\0') {
                            free(copy);
                            free(scene);
                            free(locals);
                            return LED_ERROR_PARSE;
                        }
                    }
                    if (nlocals >= clocals) {
                        uint32_t grown = (clocals == 0)
                                             ? 16u
                                             : clocals * 2u;
                        le_scene_object_id *fresh =
                            (le_scene_object_id *)realloc(
                                locals,
                                grown * sizeof(*fresh));

                        if (fresh == NULL) {
                            free(copy);
                            free(scene);
                            free(locals);
                            return LED_ERROR_OUT_OF_MEMORY;
                        }
                        locals = fresh;
                        clocals = grown;
                    }
                    locals[nlocals].hi = hi;
                    locals[nlocals].lo = lo;
                    nlocals++;
                    object_count++;
                    in_object = 1;
                    /* Copied verbatim. */
                    {
                        size_t ll2 = strlen(ln);

                        while (slen + ll2 + 2 > scap) {
                            size_t grown = scap * 2u;
                            char *fresh;

                            if (grown < scap + 64u) {
                                grown = scap + 64u;
                            }
                            if (grown >
                                LED_PREFAB_MAX_TEXT + 64u) {
                                oom = 1;
                                break;
                            }
                            fresh = (char *)realloc(scene,
                                                    grown);
                            if (fresh == NULL) {
                                oom = 1;
                                break;
                            }
                            scene = fresh;
                            scap = grown;
                        }
                        if (oom) {
                            break;
                        }
                        memcpy(scene + slen, ln, ll2);
                        slen += ll2;
                        scene[slen++] = '\n';
                        scene[slen] = '\0';
                    }
                } else if (strcmp(word, "prefab_root") == 0) {
                    const char *arg = ln + qi;
                    uint64_t hi = 0;
                    uint64_t lo = 0;

                    if (saw_root || in_object) {
                        /* Exactly one root, AFTER all objects. */
                        free(copy);
                        free(scene);
                        free(locals);
                        return LED_ERROR_PARSE;
                    }
                    while (*arg == ' ' || *arg == '\t') {
                        arg++;
                    }
                    if (!led_pf_parse_hex32(arg, &hi, &lo) ||
                        (hi == 0 && lo == 0)) {
                        free(copy);
                        free(scene);
                        free(locals);
                        return LED_ERROR_PARSE;
                    }
                    {
                        const char *tail = arg + 32;

                        while (*tail == ' ' || *tail == '\t') {
                            tail++;
                        }
                        if (*tail != '\0') {
                            free(copy);
                            free(scene);
                            free(locals);
                            return LED_ERROR_PARSE;
                        }
                    }
                    root_id.hi = hi;
                    root_id.lo = lo;
                    saw_root = 1;
                    /* Dropped from the translation. */
                } else if (strcmp(word, "asset") == 0) {
                    /* No asset table in prefabs (scenes carry
                     * relocation hints; prefabs resolve purely by
                     * ID through the registry). */
                    free(copy);
                    free(scene);
                    free(locals);
                    return LED_ERROR_PARSE;
                } else if (strcmp(word, "end") == 0) {
                    in_object = 0;
                    {
                        size_t ll2 = strlen(ln);

                        while (slen + ll2 + 2 > scap) {
                            size_t grown = scap * 2u;
                            char *fresh;

                            if (grown < scap + 64u) {
                                grown = scap + 64u;
                            }
                            if (grown >
                                LED_PREFAB_MAX_TEXT + 64u) {
                                oom = 1;
                                break;
                            }
                            fresh = (char *)realloc(scene,
                                                    grown);
                            if (fresh == NULL) {
                                oom = 1;
                                break;
                            }
                            scene = fresh;
                            scap = grown;
                        }
                        if (oom) {
                            break;
                        }
                        memcpy(scene + slen, ln, ll2);
                        slen += ll2;
                        scene[slen++] = '\n';
                        scene[slen] = '\0';
                    }
                } else {
                    /* Every other line copies verbatim (object
                     * fields AND unknown future fields — the
                     * scene parser applies the same
                     * tolerate-fields / reject-components rule,
                     * so fail-closed behavior is identical). */
                    size_t ll2 = strlen(ln);

                    while (slen + ll2 + 2 > scap) {
                        size_t grown = scap * 2u;
                        char *fresh;

                        if (grown < scap + 64u) {
                            grown = scap + 64u;
                        }
                        if (grown > LED_PREFAB_MAX_TEXT + 64u) {
                            oom = 1;
                            break;
                        }
                        fresh = (char *)realloc(scene, grown);
                        if (fresh == NULL) {
                            oom = 1;
                            break;
                        }
                        scene = fresh;
                        scap = grown;
                    }
                    if (oom) {
                        break;
                    }
                    memcpy(scene + slen, ln, ll2);
                    slen += ll2;
                    scene[slen++] = '\n';
                    scene[slen] = '\0';
                }
            }
            if (*p == '\0') {
                break;
            }
        }
    }
    free(copy);
    if (oom) {
        free(scene);
        free(locals);
        return LED_ERROR_OUT_OF_MEMORY;
    }
    /* Shape rules the scene parser cannot check. */
    if (!saw_prefab || !saw_root || object_count == 0 ||
        in_object) {
        free(scene);
        free(locals);
        return LED_ERROR_PARSE;
    }
    {
        uint32_t i;
        int found = 0;

        for (i = 0; i < nlocals; i++) {
            if (locals[i].hi == root_id.hi &&
                locals[i].lo == root_id.lo) {
                found = 1;
                break;
            }
        }
        free(locals);
        if (!found) {
            free(scene);
            return LED_ERROR_PARSE; /* root names no object */
        }
    }
    *out_scene_text = scene;
    *out_size = slen;
    if (out_prefab != NULL) {
        *out_prefab = prefab_uuid;
    }
    if (out_root != NULL) {
        *out_root = root_id;
    }
    return LED_SUCCESS;
}

/* Map a scene-parser/load error to the editor surface. */
static led_result led_pf_map_engine_error(le_result rc) {
    switch (rc) {
    case LE_SUCCESS:
        return LED_SUCCESS;
    case LE_ERROR_OUT_OF_MEMORY:
        return LED_ERROR_OUT_OF_MEMORY;
    case LE_ERROR_UNSUPPORTED_VERSION:
    case LE_ERROR_PARSE:
    case LE_ERROR_DUPLICATE_ID:
    case LE_ERROR_INVALID_HIERARCHY:
        return LED_ERROR_PARSE;
    case LE_ERROR_MISSING_ASSET:
        return LED_ERROR_VALIDATION;
    default:
        return LED_ERROR_ENGINE;
    }
}

led_result led_prefab_load(led_session *session,
                           const char *rel_path,
                           le_asset *out_asset) {
    char norm[1024];
    char abs[2048];
    char *bytes = NULL;
    size_t size = 0;
    char *scene_text = NULL;
    size_t scene_size = 0;
    led_project_asset_id prefab_uuid;
    le_scene_object_id root_id;
    led_result trc;
    le_result erc;
    le_asset validation_scene = LE_ASSET_INVALID;
    le_asset prefab = LE_ASSET_INVALID;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (session == NULL || rel_path == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!led_project_normalize(rel_path, norm)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    {
        size_t nl = strlen(norm);

        /* ".luprefab" is 9 chars (dot included). */
        if (nl < 10 || strcmp(norm + nl - 9, ".luprefab") != 0) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
    }
    if (!led_project_resolve(session, norm, abs)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_pf_read_file(abs, &bytes, &size)) {
        return LED_ERROR_IO;
    }
    memset(&prefab_uuid, 0, sizeof(prefab_uuid));
    memset(&root_id, 0, sizeof(root_id));
    trc = led_pf_translate(bytes, size, &scene_text, &scene_size,
                           &prefab_uuid, &root_id);
    free(bytes);
    if (trc != LED_SUCCESS) {
        free(scene_text);
        return trc;
    }
    /* Validate through the scene parser into a scratch scene
     * asset: full fail-closed semantics, world untouched. */
    if (le_scene_create(session->engine, 0, &validation_scene) !=
        LE_SUCCESS) {
        free(scene_text);
        return LED_ERROR_OUT_OF_MEMORY;
    }
    erc = le_scene_load_text(session->engine, &validation_scene,
                             scene_text, scene_size);
    free(scene_text);
    if (erc != LE_SUCCESS) {
        le_asset_unload(session->engine, &validation_scene);
        return led_pf_map_engine_error(erc);
    }
    /* The scratch scene holds the validated payload; the prefab
     * registry slot owns the ORIGINAL canonical bytes (engine
     * stores opaquely — instantiate re-translates from those). */
    {
        FILE *f = fopen(abs, "rb");
        char *orig = NULL;
        long n = 0;

        if (f == NULL) {
            le_asset_unload(session->engine, &validation_scene);
            return LED_ERROR_IO;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            le_asset_unload(session->engine, &validation_scene);
            return LED_ERROR_IO;
        }
        n = ftell(f);
        if (n <= 0 ||
            (uint64_t)n > LED_PREFAB_MAX_TEXT ||
            fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            le_asset_unload(session->engine, &validation_scene);
            return LED_ERROR_PARSE;
        }
        orig = (char *)malloc((size_t)n);
        if (orig == NULL) {
            fclose(f);
            le_asset_unload(session->engine, &validation_scene);
            return LED_ERROR_OUT_OF_MEMORY;
        }
        if (fread(orig, 1, (size_t)n, f) != (size_t)n) {
            free(orig);
            fclose(f);
            le_asset_unload(session->engine, &validation_scene);
            return LED_ERROR_IO;
        }
        fclose(f);
        erc = le_asset_create_prefab(session->engine, orig,
                                     (size_t)n, &prefab);
        free(orig);
        le_asset_unload(session->engine, &validation_scene);
        if (erc != LE_SUCCESS) {
            if (erc == LE_ERROR_OUT_OF_MEMORY) {
                return LED_ERROR_OUT_OF_MEMORY;
            }
            return LED_ERROR_ENGINE;
        }
    }
    if (out_asset != NULL) {
        *out_asset = prefab;
    } else {
        /* No borrower: still publish (registry owns it) — the
         * caller asked for validation only. */
    }
    return LED_SUCCESS;
}

void led_prefab_instance_free(led_prefab_instance *instance) {
    if (instance == NULL) {
        return;
    }
    free(instance->local_ids);
    free(instance->objects);
    memset(instance, 0, sizeof(*instance));
    instance->prefab_asset = LE_ASSET_INVALID;
}

/* Instantiate: translate stored prefab text -> scratch scene ->
 * validate in a scratch world FIRST (transactional: edit world
 * untouched on failure) -> commit into the edit world with prefab
 * roots attached as scene roots. */
led_result led_prefab_instantiate(led_session *session,
                                  const le_asset *prefab_asset,
                                  led_prefab_instance *out_instance) {
    const char *stored = NULL;
    size_t stored_size = 0;
    char *scene_text = NULL;
    size_t scene_size = 0;
    led_project_asset_id prefab_uuid;
    le_scene_object_id root_id;
    led_result trc;
    le_result erc;
    le_asset scratch_scene = LE_ASSET_INVALID;
    le_scene_instance committed;

    if (out_instance != NULL) {
        memset(out_instance, 0, sizeof(*out_instance));
        out_instance->prefab_asset = LE_ASSET_INVALID;
    }
    if (session == NULL || prefab_asset == NULL ||
        out_instance == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    if (!le_asset_is_alive(session->engine, prefab_asset) ||
        le_asset_get_type(session->engine, prefab_asset) !=
            LE_ASSET_PREFAB) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    stored = le_asset_get_prefab_text(session->engine,
                                      prefab_asset, &stored_size);
    if (stored == NULL || stored_size == 0) {
        return LED_ERROR_ENGINE;
    }
    memset(&prefab_uuid, 0, sizeof(prefab_uuid));
    memset(&root_id, 0, sizeof(root_id));
    memset(&committed, 0, sizeof(committed));
    trc = led_pf_translate(stored, stored_size, &scene_text,
                           &scene_size, &prefab_uuid, &root_id);
    if (trc != LED_SUCCESS) {
        free(scene_text);
        return trc;
    }
    if (le_scene_create(session->engine, 0, &scratch_scene) !=
        LE_SUCCESS) {
        free(scene_text);
        return LED_ERROR_OUT_OF_MEMORY;
    }
    erc = le_scene_load_text(session->engine, &scratch_scene,
                             scene_text, scene_size);
    free(scene_text);
    if (erc != LE_SUCCESS) {
        le_asset_unload(session->engine, &scratch_scene);
        return led_pf_map_engine_error(erc);
    }
    /* Scratch-world validation first: instantiate into a scratch
     * world so missing assets / bad hierarchy fail BEFORE the
     * edit world is touched. */
    {
        le_world *scratch = NULL;
        le_world_desc wd;
        le_scene_instance probe;

        memset(&wd, 0, sizeof(wd));
        memset(&probe, 0, sizeof(probe));
        if (le_world_create(session->engine, &wd, &scratch) !=
            LE_SUCCESS) {
            le_asset_unload(session->engine, &scratch_scene);
            return LED_ERROR_OUT_OF_MEMORY;
        }
        erc = le_scene_instantiate(scratch, &scratch_scene,
                                   &probe);
        le_scene_instance_free(&probe);
        memset(&probe, 0, sizeof(probe));
        le_world_destroy(scratch);
        if (erc != LE_SUCCESS) {
            le_asset_unload(session->engine, &scratch_scene);
            if (erc == LE_ERROR_OUT_OF_MEMORY) {
                return LED_ERROR_OUT_OF_MEMORY;
            }
            if (erc == LE_ERROR_MISSING_ASSET) {
                return LED_ERROR_VALIDATION;
            }
            if (erc == LE_ERROR_DUPLICATE_ID ||
                erc == LE_ERROR_INVALID_HIERARCHY) {
                return LED_ERROR_PARSE;
            }
            return LED_ERROR_ENGINE;
        }
    }
    /* Commit into the edit world (scene commit path: fresh
     * handles, staged validation, rollback on failure). R-015:
     * remap IDs — every instance mints fresh persistent IDs so
     * two instances never share local IDs (whole-world capture
     * would emit DUPLICATE_ID and break play/save). The published
     * local->runtime map still carries the PAYLOAD IDs verbatim
     * (le_scene_instantiate_remap contract). */
    erc = le_scene_instantiate_remap(session->edit_world,
                                     &scratch_scene, &committed,
                                     1);
    le_asset_unload(session->engine, &scratch_scene);
    if (erc != LE_SUCCESS) {
        le_scene_instance_free(&committed);
        if (erc == LE_ERROR_OUT_OF_MEMORY) {
            return LED_ERROR_OUT_OF_MEMORY;
        }
        if (erc == LE_ERROR_MISSING_ASSET) {
            return LED_ERROR_VALIDATION;
        }
        if (erc == LE_ERROR_DUPLICATE_ID ||
            erc == LE_ERROR_INVALID_HIERARCHY) {
            return LED_ERROR_PARSE;
        }
        return LED_ERROR_ENGINE;
    }
    /* Publish the editor-side instance record: prefab asset +
     * project UUID (bridged from the DB by source scan below) +
     * local->runtime map. The scene instance carries scene IDs ==
     * prefab-local IDs (translation preserves them verbatim), so
     * the map copies directly. */
    {
        led_prefab_instance *o = out_instance;
        uint32_t i;

        o->local_ids = (le_scene_object_id *)malloc(
            committed.count * sizeof(*o->local_ids));
        o->objects = (le_object *)malloc(
            committed.count * sizeof(*o->objects));
        if ((committed.count > 0 &&
             (o->local_ids == NULL || o->objects == NULL))) {
            free(o->local_ids);
            free(o->objects);
            /* Roll back the commit (transactional: failed
             * bookkeeping must not leave a half-tracked
             * instance — destroy the committed subtree by
             * root). */
            if (committed.has_root) {
                le_object_destroy(session->edit_world,
                                  &committed.root);
            }
            le_scene_instance_free(&committed);
            memset(o, 0, sizeof(*o));
            o->prefab_asset = LE_ASSET_INVALID;
            return LED_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < committed.count; i++) {
            o->local_ids[i] = committed.object_ids[i];
            o->objects[i] = committed.objects[i];
        }
        o->count = committed.count;
        /* Instance root = the object whose local ID is the
         * prefab root. */
        o->root = committed.root;
        {
            int found_root = 0;

            for (i = 0; i < committed.count; i++) {
                if (committed.object_ids[i].hi == root_id.hi &&
                    committed.object_ids[i].lo == root_id.lo) {
                    o->root = committed.objects[i];
                    found_root = 1;
                    break;
                }
            }
            (void)found_root;
        }
        o->prefab_asset = *prefab_asset;
        /* Bridge the project UUID: match the DB record whose
         * sidecar UUID equals the payload's `prefab` line. */
        memset(&o->project_id, 0, sizeof(o->project_id));
        if (session->project != NULL) {
            uint32_t r3;

            for (r3 = 0;
                 r3 < session->project->record_count; r3++) {
                if (led_project_id_equal(
                        &session->project->records[r3].id,
                        &prefab_uuid)) {
                    o->project_id =
                        session->project->records[r3].id;
                    break;
                }
            }
        }
        le_scene_instance_free(&committed);
    }
    return LED_SUCCESS;
}
