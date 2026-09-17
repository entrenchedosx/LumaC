# Phase 33 editor GUI shaders (Dear ImGui draw walk)

Canonical GLSL sources for the GUI overlay pipeline:

- `gui.vert`: ImDrawVert input (pos/uv/col) + push-constant
  scale/translate projection (upstream-backend math).
- `gui.frag`: vertex-color x texture sample (set 0 = sampled image,
  set 1 = sampler).

Build: `glslc -fshader-stage=vert gui.vert -o gui.vert.spv` (same for
frag). The compiled words are checked into
`../gui_shaders_spv.h` (generated; do not hand-edit — recompile and
re-embed). The header carries the glslc version used; committed
SPIR-V in `tests/shaders/` and `examples/*/` follows the same
discipline (binaries staged, recompiled when glslc exists).
