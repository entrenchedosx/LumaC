# Render-Graph Architecture (Phase 23)

A small renderer-owned schedule over public LumaC: passes declare
reads/writes with semantic uses; compile derives edges,
topologically sorts (declaration order breaks ties),
analyzes transient lifetimes, and executes callbacks in derived
order. Passes record their own LumaC transitions inside the
callbacks; the graph owns scheduling, transients, and
diagnostics -- never barriers, never Vulkan.

```c
lr_result lr_render_graph_create(lr_renderer *r, lr_render_graph **g);
lr_graph_pass *lr_graph_add_pass(lr_render_graph *g, const char *name,
                                 lr_graph_pass_type type,
                                 lr_graph_record_fn record, void *user);
lr_result lr_graph_pass_read(lr_graph_pass *p, lr_graph_resource *r,
                             lr_graph_use use);
lr_result lr_graph_pass_write(lr_graph_pass *p, lr_graph_resource *r,
                              lr_graph_use use);
lr_result lr_render_graph_compile(lr_render_graph *g);
lr_result lr_render_graph_execute(lr_render_graph *g,
                                  lc_command_encoder *enc);
```

Derivation rules (documented, tested): transient resources have
no initial value, so every writer precedes every reader
(producer-first) and readers without writers fail loudly;
imported resources pre-exist, so conflicting pairs follow
declaration (program) order and read/read needs no edge.
Explicit edges add semantic sequencing; cycles fail with the
passes named. Same-pass read+write is allowed when declared.
Transients allocate on compile and reuse while descriptors stay
compatible (replaced through safe retirement); lifetimes report
first/last use for future aliasing, which is intentionally not
implemented yet.

Compile caching is two-level: the visibility layer rebuilds
topology only when its key changes (flight, extent, flags, and
per-group buffer identities by stable resource ID -- never raw
pointers, so wrapper recycling cannot alias a dead schedule),
and the graph recompiles whenever its declaration signature
changes. Transients persist across both.

## Canonical proofs

`test_render_graph`: validation matrix, out-of-order producer
derivation, initial-value pattern, cycle/read-before-write/
foreign-handle rejection, transient reuse + exact peak bytes +
lifetimes, human dump, live execution order, failure
propagation. The visibility path (hiz_gen -> vis_cull ->
vis_fin -> main_draw marker) executes through the graph with
graph/manual pixel-and-stats equivalence pinned by
`test_lod_vulkan`. Diagnostics (`lr_render_graph_dump`,
`lr_graph_stats`, `lr_renderer_borrow_visibility_graph`) are
backend-neutral and MCP-ready.

## Futures

Transient aliasing from reported lifetimes, queue-class-aware
scheduling (passes already declare a class), parallel recording
of independent passes, shadow/IBL/HDR migration past the
visibility core.
