#version 450

/* Sky vertex shader (Phase 17): fullscreen triangle carrying the
 * world-space view ray. Translation cancels in the fragment
 * (ray = world - camPos), so the sky ignores camera position and
 * follows orientation only. Depth test/write stay off; the sky
 * draws first in the HDR pass and scene geometry covers it. */

layout(push_constant) uniform SkyPush {
    mat4 invVP;
    vec4 camPos;
    vec4 params; /* x = intensity, y = rotation yaw, z/w unused */
} push;

layout(location = 0) out vec3 vRay;

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec4 clip = vec4(pos * 2.0 - 1.0, 1.0, 1.0);
    vec4 world = push.invVP * clip;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    vRay = world.xyz / max(world.w, 1e-6) - push.camPos.xyz;
}
