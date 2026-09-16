/* Phase 23 indirect-count test vertex shader: fullscreen triangle
 * per command. firstVertex strides by 3, so gl_VertexIndex / 3 is
 * the command index (drives the fragment palette) and
 * gl_VertexIndex % 3 is the triangle corner. */
#version 450

layout(location = 0) flat out uint vCmd;

void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0),
                        vec2(-1.0, 3.0));
    uint v = gl_VertexIndex % 3u;

    vCmd = gl_VertexIndex / 3u;
    gl_Position = vec4(p[v], 0.0, 1.0);
}
