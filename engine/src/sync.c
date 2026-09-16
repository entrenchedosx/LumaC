/*
 * Luma Engine update + render extraction + renderer submission.
 *
 * le_world_update: advance the time accumulator, refresh dirty
 * world matrices (iterative, dirty-driven).
 *
 * le_world_extract_renderables: deterministic ascending-slot scan
 * producing plain snapshot data (world matrix + stable ID +
 * borrowed mesh/material + flags). Pure read: never touches the
 * renderer, so a future multithreaded renderer can consume the
 * snapshot off-thread.
 *
 * Submission (le_submit_frame_contents, shared by render_scene and
 * the legacy one-call render): lights + renderables for an open
 * renderer frame. Camera derivation (le_world_get_render_camera):
 * the active camera's world matrix + lens, or the documented
 * default fallback. Mesh/material liveness is NOT pre-filtered:
 * the renderer owns those registries and is the final authority
 * at submit (its rejection counts as skipped_dead).
 *
 * Camera derivation: the active camera's world matrix is
 * decomposed into position (translation column) + view (inverse
 * of the rigid part). Non-uniform scale and mirrors are handled:
 * the basis columns are normalized (mirror-safe: normalization
 * preserves the determinant sign, so mirrored views stay
 * mirrored, matching the renderer's per-item winding flip), and
 * the view is the transpose-rotation + translated eye. Projection
 * comes from the lens (perspective via lr_camera_set_perspective
 * validation rules; orthographic via the renderer's Y-flipped
 * ortho convention, reproduced engine-locally — same formula as
 * the renderer's ortho helper, documented here).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

#ifndef LE_PI
#define LE_PI 3.14159265358979323846f
#endif

le_result le_world_update(le_world *world, float dt) {
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (isfinite(dt) && dt > 0.0f) {
        world->time += (double)dt;
        le_refresh_world_matrices(world);
        /* Script lifecycle AFTER matrices (scripts read fresh
         * matrices), then refresh again for script writes. */
        le_script_step_world(world, dt);
        le_refresh_world_matrices(world);
    } else {
        /* NaN/Inf/dt<=0 drive matrices only: render_scene's
         * update(0) never runs scripts. */
        le_refresh_world_matrices(world);
    }
    return LE_SUCCESS;
}

/* Count what extraction would submit (shared by the counts query
 * and the extract path so both agree exactly). When
 * count_skipped != NULL, fills [disabled, invisible]. NOTE: the
 * scan intentionally does NOT consult mesh/material liveness (or
 * asset readiness): the renderer owns those registries and is the
 * final authority at submit (its rejection is counted as
 * skipped_dead there). */
static uint32_t le_scan_renderables(le_world *world,
                                    uint32_t *count_skipped) {
    uint32_t i;
    uint32_t n = 0;
    uint32_t skipped_disabled = 0;
    uint32_t skipped_invisible = 0;

    if (world == NULL) {
        if (count_skipped != NULL) {
            count_skipped[0] = 0;
            count_skipped[1] = 0;
        }
        return 0;
    }
    le_refresh_world_matrices(world);
    for (i = 0; i < world->capacity; i++) {
        le_object_slot *s;
        le_object handle;

        if (!world->slots[i].alive) {
            continue;
        }
        s = &world->slots[i];
        handle.index = i;
        handle.generation = s->generation;
        handle.world_tag = world->tag;
        if ((s->present & LE_PRESENT_RENDERABLE) != 0u) {
            le_renderable_desc *d;
            uint32_t idx = (uint32_t)s->renderable_index;

            if (idx >= world->renderable_count ||
                world->renderables[idx].slot != i) {
                continue;
            }
            d = &world->renderables[idx].desc;
            if (!le_object_is_effectively_enabled(world, &handle)) {
                skipped_disabled++;
                continue;
            }
            if (!d->visible) {
                skipped_invisible++;
                continue;
            }
            n++;
        } else if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
            le_asset_renderable_desc *d;
            uint32_t idx = (uint32_t)s->renderable_index;

            if (idx >= world->asset_renderable_count ||
                world->asset_renderables[idx].slot != i) {
                continue;
            }
            d = &world->asset_renderables[idx].desc;
            if (!le_object_is_effectively_enabled(world, &handle)) {
                skipped_disabled++;
                continue;
            }
            if (!d->visible) {
                skipped_invisible++;
                continue;
            }
            n++;
        }
    }
    if (count_skipped != NULL) {
        count_skipped[0] = skipped_disabled;
        count_skipped[1] = skipped_invisible;
    }
    return n;
}

void le_world_get_extraction_counts(const le_world *world,
                                    le_extraction_counts *out_counts) {
    if (out_counts == NULL) {
        return;
    }
    memset(out_counts, 0, sizeof(*out_counts));
    if (world == NULL) {
        return;
    }
    {
        uint32_t skipped[2];
        le_world *mutable_world = (le_world *)world;

        out_counts->renderables =
            le_scan_renderables(mutable_world, skipped);
        /* Lights: effectively-enabled light components. */
        {
            uint32_t i;
            uint32_t n = 0;

            for (i = 0; i < mutable_world->capacity; i++) {
                le_object_slot *s;

                if (!mutable_world->slots[i].alive) {
                    continue;
                }
                s = &mutable_world->slots[i];
                if ((s->present & LE_PRESENT_LIGHT) == 0u) {
                    continue;
                }
                if (!le_object_is_effectively_enabled(
                        mutable_world,
                        &(le_object){ i, s->generation,
                                      mutable_world->tag })) {
                    continue;
                }
                n++;
            }
            out_counts->lights = n;
        }
        if (mutable_world->has_active_camera &&
            le_object_is_alive(mutable_world,
                               &mutable_world->active_camera)) {
            uint32_t slot = mutable_world->active_camera.index;

            if (slot < mutable_world->capacity &&
                mutable_world->slots[slot].alive &&
                (mutable_world->slots[slot].present &
                 LE_PRESENT_CAMERA) != 0u) {
                out_counts->has_camera = 1;
            }
        }
    }
}

le_result le_world_extract_renderables(le_world *world,
                                       le_extracted_renderable *out_items,
                                       uint32_t capacity,
                                       uint32_t *out_count) {
    uint32_t i;
    uint32_t n = 0;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    le_refresh_world_matrices(world);
    /* Deterministic ascending-slot order (never dense order).
     * Asset-backed renderables resolve through the registry here:
     * READY assets submit their renderer backing; anything else
     * (stale/unready/wrong-type — possible only via races with
     * unload, since add validates) is skipped WITHOUT counting
     * (extraction stays a pure counting read; submit counts it as
     * skipped_dead, and both paths call the same resolver). */
    for (i = 0; i < world->capacity; i++) {
        le_object_slot *s;
        uint32_t idx;
        le_object handle;
        lr_mesh *mesh = NULL;
        lr_material *material = NULL;
        int casts_shadow = 0;
        int receives_shadow = 0;
        int visible = 0;

        if (!world->slots[i].alive) {
            continue;
        }
        s = &world->slots[i];
        if ((s->present & LE_PRESENT_RENDERABLE) != 0u) {
            le_renderable_desc *d;

            idx = (uint32_t)s->renderable_index;
            if (idx >= world->renderable_count ||
                world->renderables[idx].slot != i) {
                continue;
            }
            d = &world->renderables[idx].desc;
            mesh = d->mesh;
            material = d->material;
            casts_shadow = d->casts_shadow;
            receives_shadow = d->receives_shadow;
            visible = d->visible;
        } else if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
            le_asset_renderable_desc *d;
            le_engine *engine = world->engine;
            uint32_t mslot;
            uint32_t tslot;
            le_result code = LE_SUCCESS;

            idx = (uint32_t)s->renderable_index;
            if (idx >= world->asset_renderable_count ||
                world->asset_renderables[idx].slot != i) {
                continue;
            }
            d = &world->asset_renderables[idx].desc;
            if (engine == NULL) {
                continue;
            }
            if (!le_resolve_asset_live(engine, &d->mesh, &mslot,
                                       &code)) {
                continue;
            }
            if (!le_resolve_asset_live(engine, &d->material, &tslot,
                                       &code)) {
                continue;
            }
            if (engine->assets[mslot].type != LE_ASSET_MESH ||
                engine->assets[mslot].state != LE_ASSET_READY) {
                continue;
            }
            if (engine->assets[tslot].type != LE_ASSET_MATERIAL ||
                engine->assets[tslot].state != LE_ASSET_READY) {
                continue;
            }
            mesh = engine->assets[mslot].mesh;
            material = engine->assets[tslot].material;
            if (mesh == NULL || material == NULL) {
                continue;
            }
            casts_shadow = d->casts_shadow;
            receives_shadow = d->receives_shadow;
            visible = d->visible;
        } else {
            continue;
        }
        handle.index = i;
        handle.generation = s->generation;
        handle.world_tag = world->tag;
        if (!le_object_is_effectively_enabled(world, &handle)) {
            continue;
        }
        if (!visible) {
            continue;
        }
        if (out_items != NULL && n < capacity) {
            le_extracted_renderable *dst = &out_items[n];

            dst->object = handle;
            memcpy(dst->world_matrix, s->world_matrix,
                   sizeof(dst->world_matrix));
            dst->stable_id = le_object_stable_id(world, &handle);
            dst->mesh = mesh;
            dst->material = material;
            dst->casts_shadow = casts_shadow;
            dst->receives_shadow = receives_shadow;
            dst->mirrored = le_matrix_is_mirrored(s->world_matrix);
        }
        n++;
    }
    if (out_count != NULL) {
        *out_count = n;
    }
    return LE_SUCCESS;
}

/* Derive an lr_camera from a world matrix + lens. Position is the
 * translation column. The view inverts the rigid part: basis
 * columns are normalized (handles non-uniform scale), then the
 * view is rotation-transpose with eye translation — the standard
 * rigid inverse, mirror-preserving (a mirrored ancestor keeps a
 * negative determinant through normalization, so the derived view
 * mirrors exactly like the renderer's per-item winding flip
 * expects). Returns 0 on singular basis (caller falls back). */
static int le_camera_from_world_matrix(const float world[16],
                                       const le_camera_desc *lens,
                                       float aspect_override,
                                       lr_camera *out_camera) {
    float bx[3];
    float by[3];
    float bz[3];
    float len;
    float eye[3];
    float view[16];
    float proj[16];
    float f;
    float aspect;
    int i;

    if (world == NULL || lens == NULL || out_camera == NULL) {
        return 0;
    }
    bx[0] = world[0];
    bx[1] = world[1];
    bx[2] = world[2];
    by[0] = world[4];
    by[1] = world[5];
    by[2] = world[6];
    /* Camera forward is -Z of the world basis (looking along -Z). */
    bz[0] = -world[8];
    bz[1] = -world[9];
    bz[2] = -world[10];
    eye[0] = world[12];
    eye[1] = world[13];
    eye[2] = world[14];
    /* Normalize each axis (zero-length axis -> singular). */
    len = sqrtf(bx[0] * bx[0] + bx[1] * bx[1] + bx[2] * bx[2]);
    if (!(len > 1e-12f)) {
        return 0;
    }
    bx[0] /= len;
    bx[1] /= len;
    bx[2] /= len;
    len = sqrtf(by[0] * by[0] + by[1] * by[1] + by[2] * by[2]);
    if (!(len > 1e-12f)) {
        return 0;
    }
    by[0] /= len;
    by[1] /= len;
    by[2] /= len;
    len = sqrtf(bz[0] * bz[0] + bz[1] * bz[1] + bz[2] * bz[2]);
    if (!(len > 1e-12f)) {
        return 0;
    }
    bz[0] /= len;
    bz[1] /= len;
    bz[2] /= len;
    /* view rows are (right, up, -forward); bz IS the camera
     * forward in world coords, so view row 2 = -bz. */
    for (i = 0; i < 16; i++) {
        view[i] = 0.0f;
    }
    view[0] = bx[0];
    view[4] = bx[1];
    view[8] = bx[2];
    view[1] = by[0];
    view[5] = by[1];
    view[9] = by[2];
    view[2] = -bz[0];
    view[6] = -bz[1];
    view[10] = -bz[2];
    view[12] = -(bx[0] * eye[0] + bx[1] * eye[1] + bx[2] * eye[2]);
    view[13] = -(by[0] * eye[0] + by[1] * eye[1] + by[2] * eye[2]);
    view[14] = (bz[0] * eye[0] + bz[1] * eye[1] + bz[2] * eye[2]);
    view[15] = 1.0f;
    aspect = (aspect_override > 0.0f) ? aspect_override : lens->aspect;
    if (lens->projection == LE_PROJECTION_PERSPECTIVE) {
        if (!(lens->fov_y_rad > 0.0f) ||
            !(lens->fov_y_rad < (float)LE_PI) || !(aspect > 0.0f) ||
            !(lens->near_plane > 0.0f) ||
            !(lens->far_plane > lens->near_plane)) {
            return 0;
        }
        /* Same formula as the renderer's perspective (Y-flipped
         * Vulkan NDC): reproduced engine-locally, no internals. */
        f = 1.0f / tanf(lens->fov_y_rad * 0.5f);
        for (i = 0; i < 16; i++) {
            proj[i] = 0.0f;
        }
        proj[0] = f / aspect;
        proj[5] = -f;
        proj[10] =
            -lens->far_plane / (lens->far_plane - lens->near_plane);
        proj[11] = -1.0f;
        proj[14] = -(lens->far_plane * lens->near_plane) /
                   (lens->far_plane - lens->near_plane);
    } else {
        float h = lens->ortho_height * 0.5f;
        float w = h * aspect;
        float fn;

        if (!(lens->ortho_height > 0.0f) || !(aspect > 0.0f) ||
            !(lens->near_plane > 0.0f) ||
            !(lens->far_plane > lens->near_plane)) {
            return 0;
        }
        /* Same formula as the renderer's Y-flipped ortho. */
        fn = lens->far_plane - lens->near_plane;
        for (i = 0; i < 16; i++) {
            proj[i] = 0.0f;
        }
        proj[0] = 1.0f / w;
        proj[5] = -1.0f / h;
        proj[10] = -1.0f / fn;
        proj[12] = 0.0f;
        proj[13] = 0.0f;
        proj[14] = -lens->near_plane / fn;
        proj[15] = 1.0f;
    }
    lr_camera_init(out_camera);
    memcpy(out_camera->view, view, sizeof(view));
    memcpy(out_camera->projection, proj, sizeof(proj));
    out_camera->position[0] = eye[0];
    out_camera->position[1] = eye[1];
    out_camera->position[2] = eye[2];
    out_camera->near_plane = lens->near_plane;
    out_camera->far_plane = lens->far_plane;
    if (lens->projection == LE_PROJECTION_PERSPECTIVE) {
        out_camera->vertical_fov = lens->fov_y_rad;
        out_camera->aspect_ratio = aspect;
    } else {
        out_camera->vertical_fov = 1.0471976f;
        out_camera->aspect_ratio = aspect;
    }
    return 1;
}

/* Derive a travel-direction (renderer convention: direction the
 * light TRAVELS) from a world matrix: -Z axis, normalized. */
static int le_travel_direction(const float world[16], float out_dir[3]) {
    float d[3];
    float len;

    if (world == NULL || out_dir == NULL) {
        return 0;
    }
    d[0] = -world[8];
    d[1] = -world[9];
    d[2] = -world[10];
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-12f)) {
        return 0;
    }
    out_dir[0] = d[0] / len;
    out_dir[1] = d[1] / len;
    out_dir[2] = d[2] / len;
    return 1;
}

int le_world_get_render_camera(le_world *world, uint32_t width,
                               uint32_t height, lr_camera *out_camera) {
    lr_camera camera;
    int have_camera = 0;
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_camera == NULL) {
        return 0;
    }
    lr_camera_init(out_camera);
    if (world == NULL) {
        return 0;
    }
    le_world_update(world, 0.0f);
    /* The stored active-camera handle revalidates through the same
     * funnel as every mutating API: tag/generation mismatches (stale
     * or foreign handles) fall through to the documented default
     * camera. Slot fields are only trusted AFTER resolve succeeds. */
    if (world->has_active_camera &&
        le_resolve_live(world, &world->active_camera, &slot, &code)) {
        le_object_slot *s = &world->slots[slot];

        if ((s->present & LE_PRESENT_CAMERA) != 0u) {
            uint32_t idx = (uint32_t)s->camera_index;

            if (idx < world->camera_count &&
                world->cameras[idx].slot == slot) {
                le_camera_desc *lens = &world->cameras[idx].desc;
                float aspect = (width > 0 && height > 0)
                                   ? ((float)width / (float)height)
                                   : 0.0f;
                le_object handle = { slot, s->generation,
                                     world->tag };

                if (le_object_is_effectively_enabled(world, &handle) &&
                    le_camera_from_world_matrix(s->world_matrix, lens,
                                                aspect, &camera)) {
                    have_camera = 1;
                }
            }
        }
    }
    if (!have_camera) {
        static const float eye[3] = { 0.0f, 0.0f, 0.0f };
        static const float center[3] = { 0.0f, 0.0f, -1.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };

        lr_camera_init(&camera);
        if (lr_camera_set_perspective(&camera, 1.0471976f,
                                      (width > 0 && height > 0)
                                          ? ((float)width / (float)height)
                                          : (16.0f / 9.0f),
                                      0.1f, 1000.0f) != LR_SUCCESS) {
            return 0;
        }
        if (lr_camera_look_at(&camera, eye, center, up) != LR_SUCCESS) {
            return 0;
        }
    }
    *out_camera = camera;
    return have_camera;
}

/* Decompose an engine world matrix into an lr_transform for the
 * public renderer submit path. The common case (T*R*S chains:
 * rotation + scale, including mirrors and non-uniform scales)
 * decomposes EXACTLY into translation + quaternion + scale, so the
 * renderer's submit-time T*R*S rebuild reproduces the engine world
 * matrix up to float rounding. Residual shear from rotated
 * non-uniform ancestors has no exact rotation+scale form; the fit
 * is then the closest rigid approximation (documented, vanishingly
 * rare from T*R*S chains). Mirrors keep their sign on the
 * smallest-magnitude scale axis so the renderer's
 * determinant-keyed winding flip agrees. */
static void le_world_to_transform(const float world[16],
                                  lr_transform *out_transform) {
    float bx;
    float by;
    float bz;
    float cx;
    float cy;
    float cz;
    float dx;
    float dy;
    float dz;
    float sx;
    float sy;
    float sz;

    lr_transform_identity(out_transform);
    out_transform->position[0] = world[12];
    out_transform->position[1] = world[13];
    out_transform->position[2] = world[14];
    bx = world[0];
    by = world[1];
    bz = world[2];
    cx = world[4];
    cy = world[5];
    cz = world[6];
    dx = world[8];
    dy = world[9];
    dz = world[10];
    sx = sqrtf(bx * bx + by * by + bz * bz);
    sy = sqrtf(cx * cx + cy * cy + cz * cz);
    sz = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(sx > 1e-12f) || !(sy > 1e-12f) || !(sz > 1e-12f)) {
        /* Singular basis: translation only (the renderer
         * culls/rejects degenerate items safely). */
        return;
    }
    {
        float r00 = bx / sx;
        float r10 = by / sx;
        float r20 = bz / sx;
        float r01 = cx / sy;
        float r11 = cy / sy;
        float r21 = cz / sy;
        float r02 = dx / sz;
        float r12 = dy / sz;
        float r22 = dz / sz;
        float trace;
        float det;
        float q[4];

        det = r00 * (r11 * r22 - r12 * r21) -
              r01 * (r10 * r22 - r12 * r20) +
              r02 * (r10 * r21 - r11 * r20);
        if (det < 0.0f) {
            if (sx <= sy && sx <= sz) {
                sx = -sx;
                r00 = -r00;
                r10 = -r10;
                r20 = -r20;
            } else if (sy <= sx && sy <= sz) {
                sy = -sy;
                r01 = -r01;
                r11 = -r11;
                r21 = -r21;
            } else {
                sz = -sz;
                r02 = -r02;
                r12 = -r12;
                r22 = -r22;
            }
        }
        trace = r00 + r11 + r22;
        if (trace > 0.0f) {
            float u = sqrtf(trace + 1.0f) * 2.0f;

            q[3] = 0.25f * u;
            q[0] = (r21 - r12) / u;
            q[1] = (r02 - r20) / u;
            q[2] = (r10 - r01) / u;
        } else if (r00 > r11 && r00 > r22) {
            float u = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;

            q[3] = (r21 - r12) / u;
            q[0] = 0.25f * u;
            q[1] = (r01 + r10) / u;
            q[2] = (r02 + r20) / u;
        } else if (r11 > r22) {
            float u = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;

            q[3] = (r02 - r20) / u;
            q[0] = (r01 + r10) / u;
            q[1] = 0.25f * u;
            q[2] = (r12 + r21) / u;
        } else {
            float u = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;

            q[3] = (r10 - r01) / u;
            q[0] = (r02 + r20) / u;
            q[1] = (r12 + r21) / u;
            q[2] = 0.25f * u;
        }
        le_quat_normalize(q, out_transform->rotation);
        out_transform->scale[0] = sx;
        out_transform->scale[1] = sy;
        out_transform->scale[2] = sz;
    }
}

/* Submit lights + renderables for an open renderer frame (shared
 * by render_scene and the legacy one-call render). Updates the
 * report counters. Renderer rejections (dead/cross handles on
 * races, queue overflow past max_objects) count as skipped_dead;
 * rendering still proceeds — the renderer skips dead entries
 * defensively and the frame stays valid. Light-table overflow
 * stops further light submits but keeps rendering. */
static void le_submit_frame_contents(le_world *world, lr_renderer *renderer,
                                     uint32_t *submitted,
                                     uint32_t *skipped_disabled,
                                     uint32_t *skipped_invisible,
                                     uint32_t *skipped_dead) {
    uint32_t i;

    /* Lights first (renderer requires submits before prepare). */
    for (i = 0; i < world->capacity; i++) {
        le_object_slot *s;
        le_light_desc *d;
        uint32_t idx;
        le_object handle;
        lr_light light;

        if (!world->slots[i].alive) {
            continue;
        }
        s = &world->slots[i];
        if ((s->present & LE_PRESENT_LIGHT) == 0u) {
            continue;
        }
        idx = (uint32_t)s->light_index;
        if (idx >= world->light_count ||
            world->lights[idx].slot != i) {
            continue;
        }
        d = &world->lights[idx].desc;
        handle.index = i;
        handle.generation = s->generation;
        handle.world_tag = world->tag;
        if (!le_object_is_effectively_enabled(world, &handle)) {
            continue;
        }
        memset(&light, 0, sizeof(light));
        light.type = (lr_light_type)d->type;
        light.color[0] = d->color[0];
        light.color[1] = d->color[1];
        light.color[2] = d->color[2];
        light.intensity = d->intensity;
        light.range = d->range;
        light.spot_inner = d->spot_inner;
        light.spot_outer = d->spot_outer;
        light.shadow = d->shadow;
        if (d->type == LE_LIGHT_DIRECTIONAL ||
            d->type == LE_LIGHT_SPOT) {
            if (!le_travel_direction(s->world_matrix,
                                     light.direction)) {
                continue;
            }
        }
        if (d->type != LE_LIGHT_DIRECTIONAL) {
            light.position[0] = s->world_matrix[12];
            light.position[1] = s->world_matrix[13];
            light.position[2] = s->world_matrix[14];
        }
        if (lr_renderer_submit_light(renderer, &light) != LR_SUCCESS) {
            break;
        }
    }
    /* Renderables in deterministic ascending-slot order. Both
     * spellings resolve here: pointer-backed submits borrowed
     * handles directly; asset-backed resolves through the registry
     * (unresolvable/unready at submit time — only via unload races,
     * since add validates — count as skipped_dead with the frame
     * still valid). */
    for (i = 0; i < world->capacity; i++) {
        le_object_slot *s;
        uint32_t idx;
        le_object handle;
        lr_draw_item item;
        lr_mesh *mesh = NULL;
        lr_material *material = NULL;
        int casts_shadow = 0;
        int receives_shadow = 0;
        int visible = 0;

        if (!world->slots[i].alive) {
            continue;
        }
        s = &world->slots[i];
        if ((s->present & LE_PRESENT_RENDERABLE) != 0u) {
            le_renderable_desc *d;

            idx = (uint32_t)s->renderable_index;
            if (idx >= world->renderable_count ||
                world->renderables[idx].slot != i) {
                continue;
            }
            d = &world->renderables[idx].desc;
            mesh = d->mesh;
            material = d->material;
            casts_shadow = d->casts_shadow;
            receives_shadow = d->receives_shadow;
            visible = d->visible;
        } else if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
            le_asset_renderable_desc *d;
            le_engine *engine = world->engine;
            uint32_t mslot;
            uint32_t tslot;
            le_result code = LE_SUCCESS;

            idx = (uint32_t)s->renderable_index;
            if (idx >= world->asset_renderable_count ||
                world->asset_renderables[idx].slot != i) {
                continue;
            }
            d = &world->asset_renderables[idx].desc;
            if (engine == NULL) {
                (*skipped_dead)++;
                continue;
            }
            if (!le_resolve_asset_live(engine, &d->mesh, &mslot,
                                       &code) ||
                !le_resolve_asset_live(engine, &d->material, &tslot,
                                       &code)) {
                (*skipped_dead)++;
                continue;
            }
            if (engine->assets[mslot].type != LE_ASSET_MESH ||
                engine->assets[mslot].state != LE_ASSET_READY ||
                engine->assets[tslot].type != LE_ASSET_MATERIAL ||
                engine->assets[tslot].state != LE_ASSET_READY) {
                (*skipped_dead)++;
                continue;
            }
            mesh = engine->assets[mslot].mesh;
            material = engine->assets[tslot].material;
            if (mesh == NULL || material == NULL) {
                (*skipped_dead)++;
                continue;
            }
            casts_shadow = d->casts_shadow;
            receives_shadow = d->receives_shadow;
            visible = d->visible;
        } else {
            continue;
        }
        handle.index = i;
        handle.generation = s->generation;
        handle.world_tag = world->tag;
        if (!le_object_is_effectively_enabled(world, &handle)) {
            (*skipped_disabled)++;
            continue;
        }
        if (!visible) {
            (*skipped_invisible)++;
            continue;
        }
        if (mesh == NULL || material == NULL) {
            (*skipped_dead)++;
            continue;
        }
        memset(&item, 0, sizeof(item));
        le_world_to_transform(s->world_matrix, &item.transform);
        item.mesh = mesh;
        item.material = material;
        item.casts_shadow = casts_shadow;
        item.receives_shadow = receives_shadow;
        item.instance_id = le_object_stable_id(world, &handle);
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            (*skipped_dead)++;
            continue;
        }
        (*submitted)++;
    }
}

le_result le_world_render_scene(le_world *world,
                                lc_command_encoder *encoder, uint32_t width,
                                uint32_t height) {
    lr_renderer *renderer;
    lr_camera camera;
    lr_result lr;
    uint32_t submitted = 0;
    uint32_t skipped_disabled = 0;
    uint32_t skipped_invisible = 0;
    uint32_t skipped_dead = 0;

    if (world == NULL || encoder == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->engine == NULL || world->engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    renderer = world->engine->renderer;
    le_world_get_render_camera(world, width, height, &camera);
    lr = lr_renderer_begin(renderer, &camera);
    if (lr != LR_SUCCESS) {
        return LE_ERROR_RENDERER;
    }
    /* Zero the report up front: every exit path below (including
     * renderer failures) leaves a coherent report — the renderer
     * frame stays OPEN on failure so the caller can still
     * le_world_render_end() to reset it. */
    memset(&world->last_report, 0, sizeof(world->last_report));
    le_submit_frame_contents(world, renderer, &submitted, &skipped_disabled,
                             &skipped_invisible, &skipped_dead);
    lr = lr_renderer_render_shadows(renderer, encoder);
    if (lr != LR_SUCCESS) {
        world->last_report.submitted = submitted;
        world->last_report.skipped_disabled = skipped_disabled;
        world->last_report.skipped_invisible = skipped_invisible;
        world->last_report.skipped_dead = skipped_dead;
        lr_renderer_get_stats(renderer,
                              &world->last_report.renderer_stats);
        return LE_ERROR_RENDERER;
    }
    lr = lr_renderer_render_scene(renderer, encoder, width, height);
    if (lr != LR_SUCCESS) {
        world->last_report.submitted = submitted;
        world->last_report.skipped_disabled = skipped_disabled;
        world->last_report.skipped_invisible = skipped_invisible;
        world->last_report.skipped_dead = skipped_dead;
        lr_renderer_get_stats(renderer,
                              &world->last_report.renderer_stats);
        return LE_ERROR_RENDERER;
    }
    world->last_report.submitted = submitted;
    world->last_report.skipped_disabled = skipped_disabled;
    world->last_report.skipped_invisible = skipped_invisible;
    world->last_report.skipped_dead = skipped_dead;
    lr_renderer_get_stats(renderer, &world->last_report.renderer_stats);
    return LE_SUCCESS;
}

le_result le_world_render_output(le_world *world,
                                 lc_command_encoder *encoder,
                                 lc_render_target *target) {
    lr_renderer *renderer;

    if (world == NULL || encoder == NULL || target == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->engine == NULL || world->engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    renderer = world->engine->renderer;
    if (lr_renderer_render_output(renderer, encoder, target) !=
        LR_SUCCESS) {
        return LE_ERROR_RENDERER;
    }
    return LE_SUCCESS;
}

void le_world_render_end(le_world *world) {
    lr_renderer *renderer;

    if (world == NULL) {
        return;
    }
    if (world->engine == NULL || world->engine->renderer == NULL) {
        return;
    }
    renderer = world->engine->renderer;
    lr_renderer_get_stats(renderer, &world->last_report.renderer_stats);
    lr_renderer_end(renderer);
}

le_result le_world_render(le_world *world, lc_command_encoder *encoder,
                          lc_render_target *target, uint32_t width,
                          uint32_t height) {
    le_result res;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_image_view *view;

    if (world == NULL || encoder == NULL || target == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        width = lc_render_target_get_width(target);
        height = lc_render_target_get_height(target);
        if (width == 0 || height == 0) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    /* Scene pass first (leaves the renderer frame open). */
    res = le_world_render_scene(world, encoder, width, height);
    if (res != LE_SUCCESS) {
        /* A failed scene still holds an open renderer frame:
         * close it so the caller can continue with a clean frame.
         * The partial report from render_scene is preserved, with
         * fresh renderer stats. */
        le_world_render_end(world);
        return res;
    }
    /* Output into the caller's target inside our own pass (the
     * scene pass closed inside render_scene, so opening one here
     * is legal). Swapchain targets need the swapchain pass variant
     * owned by the caller's frame loop — use the explicit trio for
     * on-screen presentation; this one-call helper serves offscreen
     * targets. Detect the borrowed swapchain target by identity:
     * lc_swapchain_get_render_target borrows are never created via
     * lc_render_target_create, so compare against known swapchains
     * is impossible here — instead require an offscreen-capable
     * target implicitly: the explicit render pass begin rejects
     * swapchain-owned views loudly, which we map to INVALID. */
    view = lc_render_target_get_color_view(target, 0);
    if (view == NULL) {
        le_world_render_end(world);
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.0f;
    catt.clear_color[1] = 0.0f;
    catt.clear_color[2] = 0.0f;
    catt.clear_color[3] = 1.0f;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = NULL;
    pdesc.width = lc_render_target_get_width(target);
    pdesc.height = lc_render_target_get_height(target);
    if (pdesc.width == 0 || pdesc.height == 0) {
        pdesc.width = width;
        pdesc.height = height;
    }
    if (lc_encoder_begin_render_pass(encoder, &pdesc) != LC_SUCCESS) {
        /* Includes swapchain-owned views (presentation needs the
         * swapchain pass variant): mapped to INVALID per the
         * header contract. */
        le_world_render_end(world);
        return LE_ERROR_INVALID_ARGUMENT;
    }
    res = le_world_render_output(world, encoder, target);
    if (lc_encoder_end_render_pass(encoder) != LC_SUCCESS) {
        le_world_render_end(world);
        return LE_ERROR_RENDERER;
    }
    if (res != LE_SUCCESS) {
        le_world_render_end(world);
        return res;
    }
    le_world_render_end(world);
    return LE_SUCCESS;
}

void le_world_get_last_render_report(const le_world *world,
                                     le_render_report *out_report) {
    if (out_report == NULL) {
        return;
    }
    memset(out_report, 0, sizeof(*out_report));
    if (world == NULL) {
        return;
    }
    *out_report = world->last_report;
}

void le_object_get_info(const le_world *world, const le_object *object,
                        le_object_info *out_info) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->name = "";
    out_info->parent = LE_OBJECT_INVALID;
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    {
        le_object_slot *s = &world->slots[slot];
        le_object handle = { slot, s->generation, world->tag };

        out_info->alive = 1;
        out_info->enabled = s->enabled;
        out_info->effectively_enabled =
            le_object_is_effectively_enabled(world, &handle);
        if (s->parent != LE_NO_LINK && s->parent >= 0 &&
            (uint32_t)s->parent < world->capacity &&
            world->slots[s->parent].alive) {
            out_info->has_parent = 1;
            out_info->parent.index = (uint32_t)s->parent;
            out_info->parent.generation =
                world->slots[s->parent].generation;
            out_info->parent.world_tag = world->tag;
        }
        out_info->child_count = le_object_get_child_count(world, &handle);
        out_info->has_transform = 1;
        out_info->has_renderable =
            (s->present & LE_PRESENT_RENDERABLE) != 0u;
        out_info->has_camera =
            (s->present & LE_PRESENT_CAMERA) != 0u;
        out_info->has_light = (s->present & LE_PRESENT_LIGHT) != 0u;
        out_info->name =
            (s->name != NULL) ? (const char *)s->name : "";
    }
}

void le_world_get_stats(const le_world *world, le_world_stats *out_stats) {
    uint32_t i;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (world == NULL) {
        return;
    }
    out_stats->objects_alive = world->alive_count;
    out_stats->object_capacity = world->capacity;
    out_stats->enabled_objects = world->enabled_count;
    out_stats->disabled_objects =
        (world->alive_count >= world->enabled_count)
            ? (world->alive_count - world->enabled_count)
            : 0u;
    out_stats->renderables = world->renderable_count;
    out_stats->cameras = world->camera_count;
    out_stats->lights = world->light_count;
    out_stats->named_objects = world->named_count;
    out_stats->time = world->time;
    for (i = 0; i < world->capacity; i++) {
        if (world->slots[i].alive &&
            world->slots[i].parent == LE_NO_LINK) {
            out_stats->root_count++;
        }
    }
}

void le_world_get_memory_stats(const le_world *world,
                               le_memory_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (world == NULL) {
        return;
    }
    out_stats->object_slots = world->capacity;
    out_stats->object_slot_bytes =
        (uint64_t)world->capacity * sizeof(le_object_slot);
    out_stats->transform_bytes =
        (uint64_t)world->capacity * (sizeof(float) * (3u + 4u + 3u + 16u));
    out_stats->hierarchy_link_bytes =
        (uint64_t)world->capacity * (sizeof(int32_t) * 3u);
    out_stats->renderable_bytes =
        (uint64_t)world->renderable_capacity * sizeof(le_renderable_entry);
    out_stats->camera_bytes =
        (uint64_t)world->camera_capacity * sizeof(le_camera_entry);
    out_stats->light_bytes =
        (uint64_t)world->light_capacity * sizeof(le_light_entry);
    out_stats->name_bytes = world->name_bytes;
    out_stats->total_bytes =
        out_stats->object_slot_bytes + out_stats->renderable_bytes +
        out_stats->camera_bytes + out_stats->light_bytes +
        out_stats->name_bytes + sizeof(le_world);
}
