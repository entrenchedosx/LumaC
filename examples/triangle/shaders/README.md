# Triangle shaders

Original minimal shaders for the LumaC triangle example. Vertices come
from `gl_VertexIndex`; no vertex buffers involved.

- `triangle.vert` / `triangle.frag` — GLSL sources (Vulkan `#version 450`)
- `triangle.vert.spv` / `triangle.frag.spv` — committed SPIR-V used when
  no shader compiler is available at build time

Regenerate with the Vulkan SDK's `glslc` (version used: shaderc v2026.3
from SDK 1.4.357.0):

```bash
glslc -fshader-stage=vert triangle.vert -o triangle.vert.spv
glslc -fshader-stage=frag triangle.frag -o triangle.frag.spv
```

When `glslc` is found at configure time, the build recompiles the
sources automatically; otherwise the committed binaries are used.
