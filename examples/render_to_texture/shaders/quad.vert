#version 450

/* Fullscreen-triangle vertex shader (no vertex buffers): derives NDC
 * position and UV from the vertex index. Triangle covers the whole
 * target; UV (0,0)-(1,1) spans the visible area (overscan clamps). */

layout(location = 0) out vec2 fragUV;

void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    fragUV = pos;
}
