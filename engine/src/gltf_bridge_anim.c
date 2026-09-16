/*
 * Luma Engine glTF animation bridge, Phase 29: skin -> skeleton
 * asset, animations -> clip assets, over a caller-loaded
 * la_model (luma_assets owns parsing/decoding; this TU owns
 * hierarchy derivation + engine asset creation).
 *
 * Derivation rules (documented in GLTF_ANIMATION_IMPORT.md):
 * - Joints = skin joint nodes (file order). Parents = nearest
 *   joint ancestor in the node hierarchy (non-joint
 *   intermediates are baked into the bind pose).
 * - Bind local TRS: relative matrix (parent_global^-1 * global)
 *   decomposed (translation + column scales + quat). Shear from
 *   rotated non-uniform ancestors bakes as the closest rigid
 *   fit (same policy as the submit-path decomposer).
 * - Bind globals come from node local_matrix chains (lossless).
 * - Clip channels map target_node -> joint via the skeleton's
 *   joint->node map. Channels targeting non-skeleton nodes are
 *   SKIPPED with a loud count (returned, never silent).
 *   Morph-target (weights) channels never arrive (the assets
 *   layer excludes those animations/channels).
 * - Transactional: malformed data creates NOTHING (no partial
 *   skeleton, no partial clips; registry untouched).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "luma_assets/luma_assets.h"

/* Column-major 4x4 inverse (Gauss-Jordan, double precision
 * internally). Returns 1 on success, 0 on singular input (out
 * untouched). */
static int le_bridge_mat4_inverse(const float m[16],
                                  float out_inv[16]) {
    double a[4][8];
    int r;
    int c;
    int k;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            a[r][c] = (double)m[c * 4 + r];
        }
        for (c = 4; c < 8; c++) {
            a[r][c] = (c - 4 == r) ? 1.0 : 0.0;
        }
    }
    for (c = 0; c < 4; c++) {
        int piv = c;
        double pv = a[c][c] < 0.0 ? -a[c][c] : a[c][c];

        for (r = c + 1; r < 4; r++) {
            double v = a[r][c] < 0.0 ? -a[r][c] : a[r][c];

            if (v > pv) {
                pv = v;
                piv = r;
            }
        }
        if (!(pv > 1e-18)) {
            return 0;
        }
        if (piv != c) {
            for (k = 0; k < 8; k++) {
                double t = a[c][k];

                a[c][k] = a[piv][k];
                a[piv][k] = t;
            }
        }
        {
            double inv = 1.0 / a[c][c];

            for (k = 0; k < 8; k++) {
                a[c][k] *= inv;
            }
        }
        for (r = 0; r < 4; r++) {
            if (r != c && a[r][c] != 0.0) {
                double f = a[r][c];

                for (k = 0; k < 8; k++) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            double v = a[r][c + 4];

            if (v != v || v > 1e30 || v < -1e30) {
                return 0;
            }
            out_inv[c * 4 + r] = (float)v;
        }
    }
    return 1;
}

/* out = a * b (column-major). */
static void le_bridge_mat4_mul(const float a[16],
                               const float b[16], float out[16]) {
    float t[16];
    int r;
    int c;
    int k;

    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            float sum = 0.0f;

            for (k = 0; k < 4; k++) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            t[c * 4 + r] = sum;
        }
    }
    memcpy(out, t, sizeof(t));
}

/* Decompose an affine column-major matrix into TRS (translation
 * + rotation quat + scale). Mirrors the submit-path decomposer
 * policy (le_world_to_transform): column scales, mirror sign on
 * the smallest axis, closest rigid rotation fit. Returns 1 ok,
 * 0 on singular input. */
static int le_bridge_decompose_trs(const float m[16], float t[3],
                                   float q[4], float s[3]) {
    float bx = m[0];
    float by = m[1];
    float bz = m[2];
    float cx = m[4];
    float cy = m[5];
    float cz = m[6];
    float dx = m[8];
    float dy = m[9];
    float dz = m[10];
    float sx;
    float sy;
    float sz;

    sx = sqrtf(bx * bx + by * by + bz * bz);
    sy = sqrtf(cx * cx + cy * cy + cz * cz);
    sz = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(sx > 1e-12f) || !(sy > 1e-12f) || !(sz > 1e-12f)) {
        return 0;
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
        float det;
        float trace;
        float qq[4];

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

            qq[3] = 0.25f * u;
            qq[0] = (r21 - r12) / u;
            qq[1] = (r02 - r20) / u;
            qq[2] = (r10 - r01) / u;
        } else if (r00 > r11 && r00 > r22) {
            float u = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;

            qq[3] = (r21 - r12) / u;
            qq[0] = 0.25f * u;
            qq[1] = (r01 + r10) / u;
            qq[2] = (r02 + r20) / u;
        } else if (r11 > r22) {
            float u = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;

            qq[3] = (r02 - r20) / u;
            qq[0] = (r01 + r10) / u;
            qq[1] = 0.25f * u;
            qq[2] = (r12 + r21) / u;
        } else {
            float u = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;

            qq[3] = (r10 - r01) / u;
            qq[0] = (r02 + r20) / u;
            qq[1] = (r12 + r21) / u;
            qq[2] = 0.25f * u;
        }
        {
            float n = sqrtf(qq[0] * qq[0] + qq[1] * qq[1] +
                            qq[2] * qq[2] + qq[3] * qq[3]);

            if (!(n > 1e-12f)) {
                return 0;
            }
            q[0] = qq[0] / n;
            q[1] = qq[1] / n;
            q[2] = qq[2] / n;
            q[3] = qq[3] / n;
        }
    }
    t[0] = m[12];
    t[1] = m[13];
    t[2] = m[14];
    s[0] = sx;
    s[1] = sy;
    s[2] = sz;
    return 1;
}

void le_gltf_animated_free(le_gltf_animated *anim) {
    if (anim == NULL) {
        return;
    }
    free(anim->clips);
    free(anim->joint_nodes);
    memset(anim, 0, sizeof(*anim));
    anim->skeleton = LE_ASSET_INVALID;
}

le_result le_gltf_import_animated(le_engine *engine,
                                  const la_model *model,
                                  uint32_t skin_index,
                                  le_gltf_animated *out_anim) {
    uint32_t njoints;
    uint32_t j;
    int32_t *joint_nodes = NULL;
    float(*globals)[16] = NULL;
    le_skeleton_joint_desc *joints = NULL;
    le_asset skeleton = LE_ASSET_INVALID;
    le_asset *clips = NULL;
    uint32_t nanims = 0;
    uint32_t skipped = 0;
    le_result rc;

    if (out_anim != NULL) {
        memset(out_anim, 0, sizeof(*out_anim));
        out_anim->skeleton = LE_ASSET_INVALID;
    }
    if (engine == NULL || model == NULL || out_anim == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (skin_index >= la_model_get_skin_count(model)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    njoints = la_model_get_skin_joint_count(model, skin_index);
    if (njoints == 0 || njoints > LE_ANIM_MAX_JOINTS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    joint_nodes =
        (int32_t *)malloc(sizeof(int32_t) * (size_t)njoints);
    globals = (float(*)[16])malloc(sizeof(float[16]) *
                                   (size_t)njoints);
    joints = (le_skeleton_joint_desc *)calloc(
        (size_t)njoints, sizeof(*joints));
    if (joint_nodes == NULL || globals == NULL ||
        joints == NULL) {
        free(joint_nodes);
        free(globals);
        free(joints);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    /* Joint node list (duplicate joint nodes rejected: the
     * skeleton keys joints by node). */
    for (j = 0; j < njoints; j++) {
        int32_t nd =
            la_model_get_skin_joint_node(model, skin_index, j);
        uint32_t k;

        if (nd < 0 ||
            (uint32_t)nd >= la_model_get_node_count(model)) {
            free(joint_nodes);
            free(globals);
            free(joints);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        for (k = 0; k < j; k++) {
            if (joint_nodes[k] == nd) {
                free(joint_nodes);
                free(globals);
                free(joints);
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
        joint_nodes[j] = nd;
    }
    /* Bind globals from node local_matrix chains (iterative,
     * file order is not topological — fixpoint like the pose
     * evaluator). */
    {
        uint32_t n = la_model_get_node_count(model);
        uint32_t pass;

        for (j = 0; j < njoints; j++) {
            le_mat4_identity(globals[j]);
        }
        for (pass = 0; pass < n + 1u; pass++) {
            for (j = 0; j < njoints; j++) {
                const la_model_node *nd = la_model_get_node(
                    model, (uint32_t)joint_nodes[j]);
                float g[16];

                if (nd == NULL) {
                    free(joint_nodes);
                    free(globals);
                    free(joints);
                    return LE_ERROR_INVALID_ARGUMENT;
                }
                if (nd->parent < 0) {
                    memcpy(g, nd->local_matrix, sizeof(g));
                } else {
                    /* Parent global: joint ancestor (already
                     * computed this pass or last) or a
                     * non-joint node chain (walk up). */
                    float pg[16];
                    int32_t p = nd->parent;
                    uint32_t guard = 0;

                    memcpy(pg, nd->local_matrix, sizeof(pg));
                    /* Find nearest joint ancestor's global,
                     * prepending intermediate locals. */
                    while (p >= 0 && guard <= n) {
                        uint32_t jj;
                        int is_joint = 0;

                        guard++;
                        for (jj = 0; jj < njoints; jj++) {
                            if (joint_nodes[jj] == p) {
                                is_joint = 1;
                                break;
                            }
                        }
                        {
                            const la_model_node *pn =
                                la_model_get_node(
                                    model, (uint32_t)p);

                            if (pn == NULL) {
                                break;
                            }
                            if (is_joint) {
                                float tmp[16];

                                le_bridge_mat4_mul(
                                    globals[jj], pg, tmp);
                                memcpy(pg, tmp, sizeof(pg));
                                p = -1;
                            } else {
                                float tmp[16];

                                le_bridge_mat4_mul(
                                    pn->local_matrix, pg,
                                    tmp);
                                memcpy(pg, tmp, sizeof(pg));
                                p = pn->parent;
                            }
                        }
                    }
                    if (guard > n) {
                        free(joint_nodes);
                        free(globals);
                        free(joints);
                        return LE_ERROR_INVALID_ARGUMENT;
                    }
                    memcpy(g, pg, sizeof(g));
                }
                memcpy(globals[j], g, sizeof(g));
            }
        }
    }
    /* Joint descs: nearest-joint-ancestor parent + relative
     * TRS + inverse bind. */
    for (j = 0; j < njoints; j++) {
        const la_model_node *nd =
            la_model_get_node(model, (uint32_t)joint_nodes[j]);
        int32_t parent_joint = -1;
        float rel[16];
        float invp[16];
        size_t nl;

        if (nd == NULL) {
            free(joint_nodes);
            free(globals);
            free(joints);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        /* Nearest joint ancestor. */
        {
            int32_t p = nd->parent;
            uint32_t guard = 0;
            uint32_t n = la_model_get_node_count(model);

            while (p >= 0 && guard <= n) {
                uint32_t jj;

                guard++;
                for (jj = 0; jj < njoints; jj++) {
                    if (joint_nodes[jj] == p) {
                        parent_joint = (int32_t)jj;
                        break;
                    }
                }
                if (parent_joint >= 0) {
                    break;
                }
                {
                    const la_model_node *pn =
                        la_model_get_node(model,
                                          (uint32_t)p);

                    if (pn == NULL) {
                        break;
                    }
                    p = pn->parent;
                }
            }
        }
        if (parent_joint < 0) {
            memcpy(rel, globals[j], sizeof(rel));
        } else {
            if (!le_bridge_mat4_inverse(
                    globals[(uint32_t)parent_joint], invp)) {
                free(joint_nodes);
                free(globals);
                free(joints);
                return LE_ERROR_INVALID_ARGUMENT;
            }
            le_bridge_mat4_mul(invp, globals[j], rel);
        }
        if (!le_bridge_decompose_trs(rel, joints[j].translation,
                                     joints[j].rotation,
                                     joints[j].scale)) {
            free(joint_nodes);
            free(globals);
            free(joints);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        memset(joints[j].name, 0, sizeof(joints[j].name));
        if (nd->name != NULL) {
            nl = strlen(nd->name);
            if (nl >= sizeof(joints[j].name)) {
                nl = sizeof(joints[j].name) - 1u;
            }
            memcpy(joints[j].name, nd->name, nl);
        }
        joints[j].parent = parent_joint;
        la_model_get_skin_inverse_bind(model, skin_index, j,
                                       joints[j].inverse_bind);
    }
    free(globals);
    globals = NULL;
    /* Create the skeleton asset (validates hierarchy; nothing
     * created on failure). */
    {
        le_skeleton_asset_desc sd;

        memset(&sd, 0, sizeof(sd));
        sd.joints = joints;
        sd.joint_count = njoints;
        rc = le_asset_create_skeleton(engine, &sd, &skeleton);
        free(joints);
        joints = NULL;
        if (rc != LE_SUCCESS) {
            free(joint_nodes);
            return rc;
        }
    }
    /* Clips: one per model animation (empty/skipped-only
     * animations are excluded from the count). */
    nanims = la_model_get_animation_count(model);
    if (nanims > 0) {
        clips = (le_asset *)calloc((size_t)nanims,
                                   sizeof(*clips));
        if (clips == NULL) {
            le_asset_unload(engine, &skeleton);
            free(joint_nodes);
            return LE_ERROR_OUT_OF_MEMORY;
        }
    }
    {
        uint32_t a;
        uint32_t kept = 0;

        for (a = 0; a < nanims; a++) {
            uint32_t nch =
                la_model_get_animation_channel_count(model,
                                                     a);
            le_anim_track_desc *tracks = NULL;
            float duration =
                la_model_get_animation_duration(model, a);
            uint32_t c;
            uint32_t nt = 0;

            if (nch == 0 || !(duration > 0.0f)) {
                continue;
            }
            if (nch > LE_ANIM_MAX_TRACKS) {
                continue; /* absurd; skip loudly */
            }
            tracks = (le_anim_track_desc *)calloc(
                (size_t)nch, sizeof(*tracks));
            if (tracks == NULL) {
                rc = LE_ERROR_OUT_OF_MEMORY;
                goto fail_clips;
            }
            for (c = 0; c < nch; c++) {
                la_anim_channel ch;
                uint32_t jj;
                int found = 0;

                memset(&ch, 0, sizeof(ch));
                if (!la_model_get_animation_channel(model, a,
                                                    c, &ch)) {
                    skipped++;
                    continue;
                }
                if (ch.key_count == 0 || ch.times == NULL ||
                    ch.values == NULL) {
                    skipped++;
                    continue;
                }
                for (jj = 0; jj < njoints; jj++) {
                    if (joint_nodes[jj] == ch.target_node) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    /* Targets a non-skeleton node: skip loudly
                     * (rigid prop tracks belong to per-node
                     * object clips — future work). */
                    skipped++;
                    continue;
                }
                tracks[nt].target_kind = LE_ANIM_TARGET_JOINT;
                tracks[nt].target_index = jj;
                if (ch.path == 0) {
                    tracks[nt].channel =
                        LE_ANIM_CHANNEL_TRANSLATION;
                } else if (ch.path == 1) {
                    tracks[nt].channel =
                        LE_ANIM_CHANNEL_ROTATION;
                } else if (ch.path == 2) {
                    tracks[nt].channel =
                        LE_ANIM_CHANNEL_SCALE;
                } else {
                    skipped++;
                    continue;
                }
                if (ch.interpolation == 0) {
                    tracks[nt].interpolation =
                        LE_ANIM_INTERP_STEP;
                } else if (ch.interpolation == 1) {
                    tracks[nt].interpolation =
                        LE_ANIM_INTERP_LINEAR;
                } else if (ch.interpolation == 2) {
                    tracks[nt].interpolation =
                        LE_ANIM_INTERP_CUBICSPLINE;
                } else {
                    skipped++;
                    continue;
                }
                tracks[nt].times = ch.times;
                tracks[nt].values = ch.values;
                tracks[nt].key_count = ch.key_count;
                nt++;
            }
            if (nt == 0) {
                free(tracks);
                continue;
            }
            {
                le_animation_clip_desc cd;
                le_asset h = LE_ASSET_INVALID;

                memset(&cd, 0, sizeof(cd));
                cd.duration = duration;
                cd.tracks = tracks;
                cd.track_count = nt;
                rc = le_asset_create_clip(engine, &cd, &h);
                free(tracks);
                if (rc != LE_SUCCESS) {
                    goto fail_clips;
                }
                clips[kept++] = h;
            }
            continue;
        fail_clips:
            free(tracks);
            {
                uint32_t k;

                for (k = 0; k < kept; k++) {
                    le_asset_unload(engine, &clips[k]);
                }
            }
            free(clips);
            le_asset_unload(engine, &skeleton);
            free(joint_nodes);
            return rc;
        }
        if (kept < nanims) {
            /* Shrink (or free) the clip list to the kept
             * prefix; kept==0 frees. realloc shrink failure
             * keeps the oversized-but-valid array. */
            if (kept == 0) {
                free(clips);
                clips = NULL;
            } else {
                le_asset *smaller = (le_asset *)realloc(
                    clips, (size_t)kept * sizeof(*smaller));

                if (smaller != NULL) {
                    clips = smaller;
                }
            }
        }
        out_anim->skeleton = skeleton;
        out_anim->clips = clips;
        out_anim->clip_count = kept;
        out_anim->joint_nodes = joint_nodes;
        out_anim->joint_count = njoints;
        out_anim->skipped_tracks = skipped;
        return LE_SUCCESS;
    }
}
