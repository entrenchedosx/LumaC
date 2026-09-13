#version 450

/* Sky fragment shader (Phase 17): samples the base environment
 * cube along the view ray with the same yaw rotation the PBR IBL
 * uses, so lighting and sky rotate together. Linear HDR output
 * (tonemapping happens in the output pass, never here). */

layout(set = 0, binding = 0) uniform textureCube skyCube;
layout(set = 0, binding = 1) uniform sampler skySamp;

layout(push_constant) uniform SkyPush {
    mat4 invVP;
    vec4 camPos;
    vec4 params; /* x = intensity, y = rotation yaw, z/w unused */
} push;

layout(location = 0) in vec3 vRay;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 d = normalize(vRay);
    float c = cos(push.params.y);
    float s = sin(push.params.y);
    vec3 r = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
    vec3 env = texture(samplerCube(skyCube, skySamp), r).rgb;
    outColor = vec4(env * push.params.x, 1.0);
}
