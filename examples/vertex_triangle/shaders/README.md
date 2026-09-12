# Vertex-triangle shaders

`vb_triangle.vert` feeds the triangle from a real vertex buffer
(`vec2` position at location 0, `vec3` color at location 1). The
fragment stage reuses the shared passthrough shader from
`examples/triangle/shaders/triangle.frag.spv`.

`vb_triangle.vert.spv` is committed for use when no shader compiler is
available at build time. Regenerate with the Vulkan SDK's `glslc`
(version used: shaderc v2026.3 from SDK 1.4.357.0):

```bash
glslc -fshader-stage=vert vb_triangle.vert -o vb_triangle.vert.spv
```

When `glslc` is found at configure time, the build recompiles the
source automatically; otherwise the committed binary is used.
