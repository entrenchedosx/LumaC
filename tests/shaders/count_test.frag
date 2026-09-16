/* Phase 23 indirect-count test fragment shader: per-command
 * palette from push constants (painter order decides the pixel). */
#version 450

layout(location = 0) flat in uint vCmd;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    vec4 colors[4];
} push;

void main() {
    outColor = push.colors[vCmd];
}
