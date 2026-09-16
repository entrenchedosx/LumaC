/*
 * Minimal render graph (Phase 23, renderer-owned scheduling).
 *
 * Passes declare the resources they read/write with semantic uses;
 * compile derives ordering edges (write->read, write->write, and
 * read->write resolved by declaration order), topologically sorts
 * (deterministic: declaration order breaks ties), rejects cycles
 * and read-before-write transients, analyzes transient lifetimes,
 * and (re)allocates transient images/buffers with cross-frame
 * reuse. Execute runs the pass callbacks in derived order on the
 * caller's open frame encoder. Passes record their own LumaC
 * transitions inside the callbacks (public API only); the graph
 * owns scheduling, transients, and diagnostics — never barriers.
 *
 * Deliberately small: no memory aliasing (lifetimes are reported
 * for the future), no cross-queue scheduling (passes declare a
 * class for the future; execution stays dependency-ordered).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

#define LR_GRAPH_MAX_PASSES 64
#define LR_GRAPH_MAX_RESOURCES 96
#define LR_GRAPH_MAX_USES 8
#define LR_GRAPH_MAX_DEPS 8
#define LR_GRAPH_NAME 63

struct lr_graph_use_decl {
    const void *resource; /* struct lr_graph_resource *, resolved */
    lr_graph_use use;
};

struct lr_graph_pass {
    char name[LR_GRAPH_NAME + 1];
    lr_graph_pass_type type;
    lr_graph_record_fn record;
    void *user;
    struct lr_graph_use_decl reads[LR_GRAPH_MAX_USES];
    int read_count;
    struct lr_graph_use_decl writes[LR_GRAPH_MAX_USES];
    int write_count;
    const void *depends_on[LR_GRAPH_MAX_DEPS];
    int dep_count;
    int order; /* topo index after compile */
};

struct lr_graph_resource {
    char name[LR_GRAPH_NAME + 1];
    int is_image;
    int transient;
    lc_image *image;
    lc_buffer *buffer;
    lc_image_view *view; /* transient images: full-range view */
    lc_image_desc image_desc;
    lc_buffer_desc buffer_desc;
    /* Descriptor actually used to create the live transient (P1 audit
     * fix: reuse must key on the FULL descriptor — usage, samples,
     * type/flags — not just format/extent, since LumaC has no
     * usage/samples getters to re-query from the handle). */
    lc_image_desc alloc_image_desc;
    lc_buffer_desc alloc_buffer_desc;
    int has_alloc;
    int first_pass; /* topo indices, -1 when unused */
    int last_pass;
};

struct lr_render_graph {
    lr_renderer *renderer;
    struct lr_graph_pass passes[LR_GRAPH_MAX_PASSES];
    int pass_count;
    struct lr_graph_resource resources[LR_GRAPH_MAX_RESOURCES];
    int resource_count;
    int compiled;
    int dirty;
    int topo[LR_GRAPH_MAX_PASSES];
    int derived_edges;
    int derived_barriers;
    uint64_t compiled_sig;
    char error[256];
};

static void lr_graph_fail(lr_render_graph *graph, const char *msg) {
    if (graph == NULL) {
        return;
    }
    snprintf(graph->error, sizeof(graph->error), "%s", msg);
    graph->compiled = 0;
}

const char *lr_render_graph_error(const lr_render_graph *graph) {
    if (graph == NULL) {
        return "";
    }
    return graph->error;
}

lr_result lr_render_graph_create(lr_renderer *renderer,
                                 lr_render_graph **out_graph) {
    lr_render_graph *graph;

    if (renderer == NULL || out_graph == NULL) {
        if (out_graph != NULL) {
            *out_graph = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    graph = (lr_render_graph *)calloc(1, sizeof(*graph));
    if (graph == NULL) {
        *out_graph = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    graph->renderer = renderer;
    graph->dirty = 1;
    *out_graph = graph;
    return LR_SUCCESS;
}

static void lr_graph_free_transient(lr_render_graph *graph,
                                    struct lr_graph_resource *res) {
    (void)graph;
    if (res->view != NULL) {
        lc_image_view_destroy(res->view);
        res->view = NULL;
    }
    if (res->is_image) {
        lc_image_destroy(res->image);
        res->image = NULL;
    } else {
        lc_buffer_destroy(res->buffer);
        res->buffer = NULL;
    }
    res->has_alloc = 0;
}

void lr_render_graph_destroy(lr_render_graph *graph) {
    int i;

    if (graph == NULL) {
        return;
    }
    for (i = 0; i < graph->resource_count; i++) {
        struct lr_graph_resource *res = &graph->resources[i];

        if (res->transient) {
            lr_graph_free_transient(graph, res);
        }
    }
    free(graph);
}

static void lr_graph_copy_name(char *dst, const char *src) {
    size_t n;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > LR_GRAPH_NAME) {
        n = LR_GRAPH_NAME;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

lr_graph_resource *lr_graph_import_image(lr_render_graph *graph,
                                         const char *name,
                                         lc_image *image) {
    struct lr_graph_resource *res;

    if (graph == NULL || image == NULL) {
        return NULL;
    }
    if (graph->resource_count >= LR_GRAPH_MAX_RESOURCES) {
        return NULL;
    }
    res = &graph->resources[graph->resource_count++];
    memset(res, 0, sizeof(*res));
    lr_graph_copy_name(res->name, name);
    res->is_image = 1;
    res->image = image;
    res->first_pass = -1;
    res->last_pass = -1;
    graph->dirty = 1;
    return (lr_graph_resource *)res;
}

lr_graph_resource *lr_graph_import_buffer(lr_render_graph *graph,
                                          const char *name,
                                          lc_buffer *buffer) {
    struct lr_graph_resource *res;

    if (graph == NULL || buffer == NULL) {
        return NULL;
    }
    if (graph->resource_count >= LR_GRAPH_MAX_RESOURCES) {
        return NULL;
    }
    res = &graph->resources[graph->resource_count++];
    memset(res, 0, sizeof(*res));
    lr_graph_copy_name(res->name, name);
    res->is_image = 0;
    res->buffer = buffer;
    res->first_pass = -1;
    res->last_pass = -1;
    graph->dirty = 1;
    return (lr_graph_resource *)res;
}

lr_graph_resource *lr_graph_transient_image(
    lr_render_graph *graph, const char *name,
    const lc_image_desc *desc) {
    struct lr_graph_resource *res;

    if (graph == NULL || desc == NULL) {
        return NULL;
    }
    if (desc->format == LC_FORMAT_UNDEFINED || desc->width == 0 ||
        desc->height == 0 || desc->array_layers == 0 ||
        desc->usage == 0) {
        return NULL;
    }
    if (graph->resource_count >= LR_GRAPH_MAX_RESOURCES) {
        return NULL;
    }
    res = &graph->resources[graph->resource_count++];
    memset(res, 0, sizeof(*res));
    lr_graph_copy_name(res->name, name);
    res->is_image = 1;
    res->transient = 1;
    res->image_desc = *desc;
    if (res->image_desc.mip_levels == 0) {
        res->image_desc.mip_levels = 1;
    }
    res->first_pass = -1;
    res->last_pass = -1;
    graph->dirty = 1;
    return (lr_graph_resource *)res;
}

lr_graph_resource *lr_graph_transient_buffer(
    lr_render_graph *graph, const char *name,
    const lc_buffer_desc *desc) {
    struct lr_graph_resource *res;

    if (graph == NULL || desc == NULL) {
        return NULL;
    }
    if (desc->size == 0 || desc->usage == 0) {
        return NULL;
    }
    if (graph->resource_count >= LR_GRAPH_MAX_RESOURCES) {
        return NULL;
    }
    res = &graph->resources[graph->resource_count++];
    memset(res, 0, sizeof(*res));
    lr_graph_copy_name(res->name, name);
    res->is_image = 0;
    res->transient = 1;
    res->buffer_desc = *desc;
    res->first_pass = -1;
    res->last_pass = -1;
    graph->dirty = 1;
    return (lr_graph_resource *)res;
}

lc_image *lr_graph_resource_get_image(lr_graph_resource *resource) {
    struct lr_graph_resource *res =
        (struct lr_graph_resource *)resource;

    if (res == NULL || !res->is_image) {
        return NULL;
    }
    return res->image;
}

lc_buffer *lr_graph_resource_get_buffer(lr_graph_resource *resource) {
    struct lr_graph_resource *res =
        (struct lr_graph_resource *)resource;

    if (res == NULL || res->is_image) {
        return NULL;
    }
    return res->buffer;
}

lr_graph_pass *lr_graph_add_pass(lr_render_graph *graph,
                                 const char *name,
                                 lr_graph_pass_type type,
                                 lr_graph_record_fn record,
                                 void *user) {
    struct lr_graph_pass *pass;

    if (graph == NULL || record == NULL) {
        return NULL;
    }
    if (type != LR_GRAPH_PASS_GRAPHICS &&
        type != LR_GRAPH_PASS_COMPUTE &&
        type != LR_GRAPH_PASS_TRANSFER) {
        return NULL;
    }
    if (graph->pass_count >= LR_GRAPH_MAX_PASSES) {
        return NULL;
    }
    pass = &graph->passes[graph->pass_count++];
    memset(pass, 0, sizeof(*pass));
    lr_graph_copy_name(pass->name, name);
    pass->type = type;
    pass->record = record;
    pass->user = user;
    pass->order = -1;
    graph->dirty = 1;
    return (lr_graph_pass *)pass;
}

lr_result lr_graph_pass_read(lr_graph_pass *pass,
                             lr_graph_resource *resource,
                             lr_graph_use use) {
    struct lr_graph_pass *p = (struct lr_graph_pass *)pass;
    int i;

    if (p == NULL || resource == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < p->read_count; i++) {
        if (p->reads[i].resource == (const void *)resource) {
            p->reads[i].use = use;
            return LR_SUCCESS;
        }
    }
    if (p->read_count >= LR_GRAPH_MAX_USES) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    p->reads[p->read_count].resource = (const void *)resource;
    p->reads[p->read_count].use = use;
    p->read_count++;
    return LR_SUCCESS;
}

lr_result lr_graph_pass_write(lr_graph_pass *pass,
                              lr_graph_resource *resource,
                              lr_graph_use use) {
    struct lr_graph_pass *p = (struct lr_graph_pass *)pass;
    int i;

    if (p == NULL || resource == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < p->write_count; i++) {
        if (p->writes[i].resource == (const void *)resource) {
            p->writes[i].use = use;
            return LR_SUCCESS;
        }
    }
    if (p->write_count >= LR_GRAPH_MAX_USES) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    p->writes[p->write_count].resource = (const void *)resource;
    p->writes[p->write_count].use = use;
    p->write_count++;
    return LR_SUCCESS;
}

lr_result lr_graph_add_dependency(lr_graph_pass *before,
                                  lr_graph_pass *after) {
    struct lr_graph_pass *after_p = (struct lr_graph_pass *)after;
    int i;

    if (before == NULL || after == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (before == after) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < after_p->dep_count; i++) {
        if (after_p->depends_on[i] == (const void *)before) {
            return LR_SUCCESS;
        }
    }
    if (after_p->dep_count >= LR_GRAPH_MAX_DEPS) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    after_p->depends_on[after_p->dep_count++] =
        (const void *)before;
    return LR_SUCCESS;
}

/* Resolve a resource pointer to a graph-local index (-1 foreign). */
static int lr_graph_resource_index(const lr_render_graph *graph,
                                   const void *resource) {
    int i;

    for (i = 0; i < graph->resource_count; i++) {
        if ((const void *)&graph->resources[i] == resource) {
            return i;
        }
    }
    return -1;
}

static int lr_graph_pass_index(const lr_render_graph *graph,
                               const void *pass) {
    int i;

    for (i = 0; i < graph->pass_count; i++) {
        if ((const void *)&graph->passes[i] == pass) {
            return i;
        }
    }
    return -1;
}

/* Estimated transient bytes (documented upper bound; exact
 * suballocation stays behind LumaC). */
static uint64_t lr_graph_format_bytes(lc_format format) {
    switch (format) {
    case LC_FORMAT_R8_UNORM:
        return 1;
    case LC_FORMAT_RG8_UNORM:
    case LC_FORMAT_R16_FLOAT:
    case LC_FORMAT_D16_UNORM:
        return 2;
    case LC_FORMAT_RGBA8_UNORM:
    case LC_FORMAT_RGBA8_SRGB:
    case LC_FORMAT_BGRA8_UNORM:
    case LC_FORMAT_BGRA8_SRGB:
    case LC_FORMAT_RG16_FLOAT:
    case LC_FORMAT_R32_FLOAT:
    case LC_FORMAT_R32_UINT:
    case LC_FORMAT_D32_FLOAT:
    case LC_FORMAT_D24_UNORM_S8_UINT:
        return 4;
    case LC_FORMAT_RG32_FLOAT:
    case LC_FORMAT_RGBA16_FLOAT:
    case LC_FORMAT_RG32_UINT:
        return 8;
    case LC_FORMAT_RGB32_FLOAT:
    case LC_FORMAT_RGB32_UINT:
        return 12;
    case LC_FORMAT_RGBA32_FLOAT:
    case LC_FORMAT_RGBA32_UINT:
        return 16;
    default:
        return 4;
    }
}

static int lr_graph_format_is_depth(lc_format format) {
    return (format == LC_FORMAT_D16_UNORM ||
            format == LC_FORMAT_D24_UNORM_S8_UINT ||
            format == LC_FORMAT_D32_FLOAT)
               ? 1
               : 0;
}

static int lr_graph_image_compatible(const lc_image_desc *want,
                                     const lc_image *have,
                                     const lc_image_desc *alloc) {
    if (have == NULL || alloc == NULL) {
        return 0;
    }
    /* Full-descriptor reuse key (P1 audit fix): usage/samples/type/flags
     * changes must recreate, not silently reuse a wrongly-created image.
     * Geometry is cross-checked against both the live handle (cheap
     * getters) and the recorded allocation descriptor. */
    if (lc_image_get_format(have) != want->format ||
        lc_image_get_width(have) != want->width ||
        lc_image_get_height(have) != want->height ||
        lc_image_get_mip_levels(have) != want->mip_levels ||
        lc_image_get_array_layers(have) != want->array_layers) {
        return 0;
    }
    return (alloc->format == want->format && alloc->width == want->width &&
            alloc->height == want->height &&
            alloc->depth == want->depth &&
            alloc->mip_levels == want->mip_levels &&
            alloc->array_layers == want->array_layers &&
            alloc->usage == want->usage && alloc->flags == want->flags &&
            alloc->samples == want->samples &&
            alloc->type == want->type)
               ? 1
               : 0;
}

static int lr_graph_buffer_compatible(const lc_buffer_desc *want,
                                      const lc_buffer *have,
                                      const lc_buffer_desc *alloc,
                                      uint64_t *out_size) {
    uint64_t size = 0;

    if (have == NULL || alloc == NULL) {
        return 0;
    }
    size = lc_buffer_get_size(have);
    if (out_size != NULL) {
        *out_size = size;
    }
    /* Size is checked against the live handle; usage/memory against the
     * recorded allocation descriptor (no public getters exist). */
    return (size == want->size && alloc->size == want->size &&
            alloc->usage == want->usage &&
            alloc->memory == want->memory)
               ? 1
               : 0;
}

static lr_result lr_graph_alloc_transient(lr_render_graph *graph,
                                          struct lr_graph_resource *res) {
    if (res->is_image) {
        lc_image_view_desc vdesc;

        if (res->has_alloc &&
            lr_graph_image_compatible(&res->image_desc, res->image,
                                      &res->alloc_image_desc) &&
            res->view != NULL) {
            return LR_SUCCESS; /* reused across frames */
        }
        lr_graph_free_transient(graph, res);
        res->has_alloc = 0;
        if (lc_image_create(graph->renderer->device,
                            &res->image_desc, &res->image) !=
            LC_SUCCESS) {
            lr_graph_fail(graph, "transient image allocation failed");
            return LR_ERROR_RENDER;
        }
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = lr_graph_format_is_depth(res->image_desc.format)
                           ? LC_IMAGE_ASPECT_DEPTH
                           : LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = res->image_desc.mip_levels;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = res->image_desc.array_layers;
        if (lc_image_view_create(res->image, &vdesc, &res->view) !=
            LC_SUCCESS) {
            lr_graph_free_transient(graph, res);
            res->has_alloc = 0;
            lr_graph_fail(graph, "transient image view failed");
            return LR_ERROR_RENDER;
        }
        res->alloc_image_desc = res->image_desc;
        res->has_alloc = 1;
        return LR_SUCCESS;
    }
    {
        uint64_t size = 0;

        if (res->has_alloc &&
            lr_graph_buffer_compatible(&res->buffer_desc, res->buffer,
                                       &res->alloc_buffer_desc, &size)) {
            return LR_SUCCESS; /* reused across frames */
        }
        lr_graph_free_transient(graph, res);
        res->has_alloc = 0;
        if (lc_buffer_create(graph->renderer->device,
                             &res->buffer_desc, &res->buffer) !=
            LC_SUCCESS) {
            lr_graph_fail(graph, "transient buffer allocation failed");
            return LR_ERROR_RENDER;
        }
        res->alloc_buffer_desc = res->buffer_desc;
        res->has_alloc = 1;
        return LR_SUCCESS;
    }
}

/* Topology signature: any declaration change alters it, so
 * execute notices mutations that bypass the dirty flag (reads,
 * writes, and explicit edges have no graph pointer to mark
 * through). Cheap FNV over the declaration sets. */
static uint64_t lr_graph_signature(const lr_render_graph *graph) {
    uint64_t key = 1469598103934665603ull;
    int i;
    int k;

#define LR_GMIX(v)                                        \
    do {                                                  \
        key ^= (uint64_t)(v);                             \
        key *= 1099511628211ull;                          \
    } while (0)

    LR_GMIX(graph->pass_count);
    LR_GMIX(graph->resource_count);
    for (i = 0; i < graph->pass_count; i++) {
        const struct lr_graph_pass *p = &graph->passes[i];

        LR_GMIX(p->type);
        LR_GMIX(p->read_count);
        LR_GMIX(p->write_count);
        LR_GMIX(p->dep_count);
        for (k = 0; k < p->read_count; k++) {
            LR_GMIX(p->reads[k].resource);
            LR_GMIX(p->reads[k].use);
        }
        for (k = 0; k < p->write_count; k++) {
            LR_GMIX(p->writes[k].resource);
            LR_GMIX(p->writes[k].use);
        }
        for (k = 0; k < p->dep_count; k++) {
            LR_GMIX(p->depends_on[k]);
        }
    }
    for (i = 0; i < graph->resource_count; i++) {
        const struct lr_graph_resource *r = &graph->resources[i];

        LR_GMIX(r->is_image);
        LR_GMIX(r->transient);
        /* Import handles join the signature so a swapped handle
         * (same declarations, different backing object) forces a
         * recompile instead of silently reusing the order. */
        LR_GMIX(r->image);
        LR_GMIX(r->buffer);
    }
#undef LR_GMIX
    return key;
}

lr_result lr_render_graph_compile(lr_render_graph *graph) {
    /* adjacency as bit sets over pass indices (64 max). */
    uint64_t succ[LR_GRAPH_MAX_PASSES];
    int indegree[LR_GRAPH_MAX_PASSES];
    int queue[LR_GRAPH_MAX_PASSES];
    int order[LR_GRAPH_MAX_PASSES];
    int placed = 0;
    int i;
    int r;

    if (graph == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    graph->error[0] = '\0';
    graph->derived_edges = 0;
    graph->derived_barriers = 0;
    memset(succ, 0, sizeof(succ));
    memset(indegree, 0, sizeof(indegree));
    for (i = 0; i < graph->pass_count; i++) {
        graph->passes[i].order = -1;
    }
    for (r = 0; r < graph->resource_count; r++) {
        graph->resources[r].first_pass = -1;
        graph->resources[r].last_pass = -1;
    }
    /* Validate declarations: every use must name a resource of
     * THIS graph; every explicit edge must join two passes of
     * THIS graph. */
    for (i = 0; i < graph->pass_count; i++) {
        struct lr_graph_pass *p = &graph->passes[i];
        int k;

        for (k = 0; k < p->read_count; k++) {
            if (lr_graph_resource_index(
                    graph, p->reads[k].resource) < 0) {
                lr_graph_fail(graph, "pass reads a foreign resource");
                return LR_ERROR_INVALID_ARGUMENT;
            }
        }
        for (k = 0; k < p->write_count; k++) {
            if (lr_graph_resource_index(
                    graph, p->writes[k].resource) < 0) {
                lr_graph_fail(graph, "pass writes a foreign resource");
                return LR_ERROR_INVALID_ARGUMENT;
            }
        }
        for (k = 0; k < p->dep_count; k++) {
            if (lr_graph_pass_index(graph, p->depends_on[k]) < 0) {
                lr_graph_fail(graph, "explicit edge names a foreign pass");
                return LR_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    /* Derive edges. Two rules, documented in
     * RENDER_GRAPH_ARCHITECTURE.md:
     * - TRANSIENT resources have no initial value: every writer
     *   precedes every reader (producer-first), writers chain in
     *   declaration order. A reader with no earlier writer is a
     *   compile error (checked below).
     * - IMPORTED resources pre-exist: conflicting pairs order by
     *   declaration (program order), so "read the incoming value,
     *   then update" and "produce, then consume" both express.
     * Read/read pairs never need an edge. Explicit edges join
     * unconditionally. */
    for (r = 0; r < graph->resource_count; r++) {
        int touches[LR_GRAPH_MAX_PASSES];
        int is_write[LR_GRAPH_MAX_PASSES];
        int count = 0;
        int a;
        int b;

        for (i = 0; i < graph->pass_count; i++) {
            struct lr_graph_pass *p = &graph->passes[i];
            int read = 0;
            int write = 0;
            int k;

            for (k = 0; k < p->read_count; k++) {
                if (lr_graph_resource_index(
                        graph, p->reads[k].resource) == r) {
                    read = 1;
                }
            }
            for (k = 0; k < p->write_count; k++) {
                if (lr_graph_resource_index(
                        graph, p->writes[k].resource) == r) {
                    write = 1;
                }
            }
            if (read || write) {
                touches[count] = i;
                is_write[count] = write;
                count++;
            }
        }
        if (graph->resources[r].transient) {
            for (a = 0; a < count; a++) {
                for (b = 0; b < count; b++) {
                    int pa = touches[a];
                    int pb = touches[b];
                    int edge = 0;
                    int barrier = 0;

                    if (is_write[a] && !is_write[b]) {
                        edge = 1; /* producer before consumer */
                        barrier = 1;
                    } else if (is_write[a] && is_write[b] &&
                               a < b) {
                        edge = 1; /* writer chain */
                    }
                    if (edge &&
                        (succ[pa] &
                         ((uint64_t)1 << (uint64_t)pb)) == 0) {
                        succ[pa] |=
                            ((uint64_t)1 << (uint64_t)pb);
                        indegree[pb]++;
                        graph->derived_edges++;
                        if (barrier) {
                            graph->derived_barriers++;
                        }
                    }
                }
            }
            continue;
        }
        for (a = 0; a < count; a++) {
            for (b = a + 1; b < count; b++) {
                int pa = touches[a];
                int pb = touches[b];

                if (!is_write[a] && !is_write[b]) {
                    continue; /* read/read: no edge */
                }
                if ((succ[pa] & ((uint64_t)1 << (uint64_t)pb)) ==
                    0) {
                    succ[pa] |=
                        ((uint64_t)1 << (uint64_t)pb);
                    indegree[pb]++;
                    graph->derived_edges++;
                    if (is_write[a]) {
                        /* A producer/consumer transition is
                         * required between these passes. */
                        graph->derived_barriers++;
                    }
                }
            }
        }
    }
    for (i = 0; i < graph->pass_count; i++) {
        struct lr_graph_pass *p = &graph->passes[i];
        int k;

        for (k = 0; k < p->dep_count; k++) {
            int before = lr_graph_pass_index(graph, p->depends_on[k]);

            if (before >= 0 && before != i &&
                (succ[before] & ((uint64_t)1 << (uint64_t)i)) == 0) {
                succ[before] |= ((uint64_t)1 << (uint64_t)i);
                indegree[i]++;
                graph->derived_edges++;
            }
        }
    }
    /* Kahn's algorithm, declaration order breaking ties: the
     * execution order is DERIVED, never the insertion order. */
    {
        int head = 0;
        int tail = 0;

        for (i = 0; i < graph->pass_count; i++) {
            if (indegree[i] == 0) {
                queue[tail++] = i;
            }
        }
        while (head < tail) {
            /* Smallest declaration index first (queue stays
             * sorted: indices only ever appended in order... not
             * guaranteed, so select the minimum explicitly). */
            int best = head;
            int n;

            for (n = head + 1; n < tail; n++) {
                if (queue[n] < queue[best]) {
                    best = n;
                }
            }
            {
                int pickup = queue[best];

                queue[best] = queue[head];
                queue[head] = pickup;
            }
            {
                int cur = queue[head++];
                int next;

                order[placed++] = cur;
                for (next = 0; next < graph->pass_count; next++) {
                    if ((succ[cur] &
                         ((uint64_t)1 << (uint64_t)next)) != 0) {
                        if (--indegree[next] == 0) {
                            queue[tail++] = next;
                        }
                    }
                }
            }
        }
    }
    if (placed != graph->pass_count) {
        char msg[256];
        int n = 0;

        n += snprintf(msg + n, sizeof(msg) - (size_t)n,
                      "dependency cycle between passes:");
        for (i = 0; i < graph->pass_count && n < 200; i++) {
            if (indegree[i] > 0) {
                n += snprintf(msg + n, sizeof(msg) - (size_t)n,
                              " %s", graph->passes[i].name);
            }
        }
        lr_graph_fail(graph, msg);
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < placed; i++) {
        graph->topo[i] = order[i];
        graph->passes[order[i]].order = i;
    }
    /* Read-before-write: a transient read with no writer earlier
     * in derived order is a compile error (imported resources are
     * pre-existing, so their reads are always legal). Lifetimes
     * span first..last derived use (upper bound for future
     * aliasing: no sharing is performed yet). */
    for (r = 0; r < graph->resource_count; r++) {
        struct lr_graph_resource *res = &graph->resources[r];
        int writer = -1;
        int reader = -1;
        int t;

        res->first_pass = -1;
        res->last_pass = -1;
        for (t = 0; t < placed; t++) {
            struct lr_graph_pass *p = &graph->passes[graph->topo[t]];
            int touches = 0;
            int k;

            for (k = 0; k < p->write_count; k++) {
                if (lr_graph_resource_index(
                        graph, p->writes[k].resource) == r) {
                    touches = 1;
                    if (writer < 0) {
                        writer = t;
                    }
                }
            }
            for (k = 0; k < p->read_count; k++) {
                if (lr_graph_resource_index(
                        graph, p->reads[k].resource) == r) {
                    touches = 1;
                    if (reader < 0) {
                        reader = t;
                    }
                }
            }
            if (touches) {
                if (res->first_pass < 0) {
                    res->first_pass = t;
                }
                res->last_pass = t;
            }
        }
        if (res->transient && reader >= 0 &&
            (writer < 0 || reader < writer)) {
            char msg[256];

            snprintf(msg, sizeof(msg),
                     "transient '%s' read before first write",
                     res->name);
            lr_graph_fail(graph, msg);
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    /* Transient lifetimes (topo indices) + allocation/reuse. */
    for (r = 0; r < graph->resource_count; r++) {
        struct lr_graph_resource *res = &graph->resources[r];

        if (!res->transient) {
            continue;
        }
        if (lr_graph_alloc_transient(graph, res) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    graph->compiled = 1;
    graph->dirty = 0;
    graph->compiled_sig = lr_graph_signature(graph);
    return LR_SUCCESS;
}

lr_result lr_render_graph_execute(lr_render_graph *graph,
                                  lc_command_encoder *encoder) {
    int i;

    if (graph == NULL || encoder == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!graph->compiled || graph->dirty ||
        lr_graph_signature(graph) != graph->compiled_sig) {
        if (lr_render_graph_compile(graph) != LR_SUCCESS) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < graph->pass_count; i++) {
        struct lr_graph_pass *p = &graph->passes[graph->topo[i]];
        lr_result res = p->record(graph->renderer, encoder, p->user);

        if (res != LR_SUCCESS) {
            char msg[256];

            snprintf(msg, sizeof(msg), "pass '%s' failed", p->name);
            lr_graph_fail(graph, msg);
            return res;
        }
    }
    return LR_SUCCESS;
}

void lr_render_graph_get_stats(const lr_render_graph *graph,
                               lr_graph_stats *out_stats) {
    lr_graph_stats stats;
    int i;

    memset(&stats, 0, sizeof(stats));
    if (graph != NULL && graph->compiled) {
        stats.pass_count = (uint32_t)graph->pass_count;
        stats.resource_count = (uint32_t)graph->resource_count;
        stats.dependency_edges = (uint32_t)graph->derived_edges;
        stats.derived_barriers = (uint32_t)graph->derived_barriers;
        for (i = 0; i < graph->pass_count; i++) {
            if (graph->passes[i].type == LR_GRAPH_PASS_GRAPHICS) {
                stats.graphics_passes++;
            } else if (graph->passes[i].type ==
                       LR_GRAPH_PASS_COMPUTE) {
                stats.compute_passes++;
            } else {
                stats.transfer_passes++;
            }
        }
        for (i = 0; i < graph->resource_count; i++) {
            const struct lr_graph_resource *res =
                &graph->resources[i];

            if (!res->transient) {
                continue;
            }
            stats.transient_resources++;
            if (res->is_image) {
                uint64_t px =
                    (uint64_t)res->image_desc.width *
                    (uint64_t)res->image_desc.height;
                uint64_t bpp = lr_graph_format_bytes(
                    res->image_desc.format);

                stats.peak_transient_bytes +=
                    px * bpp *
                    (uint64_t)res->image_desc.mip_levels;
            } else {
                stats.peak_transient_bytes += res->buffer_desc.size;
            }
        }
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
}

static const char *lr_graph_use_name(lr_graph_use use) {
    switch (use) {
    case LR_GRAPH_USE_SAMPLED_READ:
        return "sampled-read";
    case LR_GRAPH_USE_STORAGE_READ:
        return "storage-read";
    case LR_GRAPH_USE_STORAGE_WRITE:
        return "storage-write";
    case LR_GRAPH_USE_COLOR_ATTACHMENT:
        return "color-attachment";
    case LR_GRAPH_USE_DEPTH_ATTACHMENT:
        return "depth-attachment";
    case LR_GRAPH_USE_INDIRECT_READ:
        return "indirect-read";
    case LR_GRAPH_USE_TRANSFER_READ:
        return "transfer-read";
    case LR_GRAPH_USE_TRANSFER_WRITE:
        return "transfer-write";
    default:
        return "unknown";
    }
}

static const char *lr_graph_type_name(lr_graph_pass_type type) {
    switch (type) {
    case LR_GRAPH_PASS_GRAPHICS:
        return "graphics";
    case LR_GRAPH_PASS_COMPUTE:
        return "compute";
    case LR_GRAPH_PASS_TRANSFER:
        return "transfer";
    default:
        return "unknown";
    }
}

size_t lr_render_graph_dump(const lr_render_graph *graph, char *buf,
                            size_t cap) {
    /* Two passes: measure, then optionally fill. */
    size_t need = 0;
    int i;
    int k;

#define LR_GRAPH_EMIT(...)                                            \
    do {                                                              \
        char tmp[256];                                                \
        int n = snprintf(tmp, sizeof(tmp), __VA_ARGS__);              \
        if (n < 0) {                                                  \
            break;                                                    \
        }                                                             \
        if (buf != NULL && need < cap) {                              \
            size_t room = cap - need;                                 \
            size_t take = ((size_t)n < room) ? (size_t)n : room;      \
            memcpy(buf + need, tmp, take);                            \
            need += take;                                             \
        } else {                                                      \
            need += (size_t)n;                                        \
        }                                                             \
    } while (0)

    if (graph == NULL) {
        return 0;
    }
    LR_GRAPH_EMIT("graph: %d passes, %d resources%s\n",
                  graph->pass_count, graph->resource_count,
                  graph->compiled ? " (compiled)" : " (uncompiled)");
    for (i = 0; i < graph->pass_count; i++) {
        const struct lr_graph_pass *p =
            &graph->passes[graph->compiled ? graph->topo[i] : i];

        LR_GRAPH_EMIT("pass[%d] %s (%s)\n", i, p->name,
                      lr_graph_type_name(p->type));
        for (k = 0; k < p->read_count; k++) {
            int r = lr_graph_resource_index(
                graph, p->reads[k].resource);

            LR_GRAPH_EMIT("  reads %s (%s)\n",
                          (r >= 0) ? graph->resources[r].name : "?",
                          lr_graph_use_name(p->reads[k].use));
        }
        for (k = 0; k < p->write_count; k++) {
            int r = lr_graph_resource_index(
                graph, p->writes[k].resource);

            LR_GRAPH_EMIT("  writes %s (%s)\n",
                          (r >= 0) ? graph->resources[r].name : "?",
                          lr_graph_use_name(p->writes[k].use));
        }
    }
    for (i = 0; i < graph->resource_count; i++) {
        const struct lr_graph_resource *res = &graph->resources[i];

        if (res->transient) {
            LR_GRAPH_EMIT("resource %s: transient %s [first %d, "
                          "last %d]\n",
                          res->name,
                          res->is_image ? "image" : "buffer",
                          res->first_pass, res->last_pass);
        } else {
            LR_GRAPH_EMIT("resource %s: imported %s\n", res->name,
                          res->is_image ? "image" : "buffer");
        }
    }
    if (buf != NULL && cap > 0) {
        if (need < cap) {
            buf[need] = '\0';
        } else {
            buf[cap - 1] = '\0';
        }
    }
    return need;
#undef LR_GRAPH_EMIT
}
