#version 450

/* Editor environment sky vertex shader (R-012): fullscreen triangle
 * carrying the world-space view ray. Same math as sky.vert —
 * translation cancels in the fragment (ray = world - camPos), so the
 * gradient ignores camera position and follows orientation only.
 * Depth test/write stay off; the pass draws first in the HDR scene
 * pass and scene geometry covers it. */

layout(push_constant) uniform EditorSkyPush {
    mat4 invVP;
    vec4 camPos;
    vec4 zoneA; /* xyz = zenith color, w = unused */
    vec4 zoneB; /* xyz = horizon color, w = unused */
    vec4 zoneC; /* xyz = nadir color, w = horizon softness */
} push;

layout(location = 0) out vec3 vRay;

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec4 clip = vec4(pos * 2.0 - 1.0, 1.0, 1.0);
    vec4 world = push.invVP * clip;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    vRay = world.xyz / max(world.w, 1e-6) - push.camPos.xyz;
}
