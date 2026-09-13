#version 450

/* Shared fullscreen-triangle vertex shader for renderer post and
 * preprocessing passes (no vertex buffers): derives NDC position
 * and memory-order UV (vUV.y = 0 is the first image row) from the
 * vertex index. Follows examples/render_to_texture/shaders/quad.vert.
 * Triangle covers the whole target; overscan clamps. */

layout(location = 0) out vec2 vUV;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    vUV = pos;
}
