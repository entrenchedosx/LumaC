#version 450

/* Equirectangular HDR source -> one cubemap face (Phase 17).
 * Push selects the face (0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z).
 * Face-write mapping mirrors the Vulkan cube sampling convention
 * (spec face selection/sc/tc), with memory UV (0,0) at the first
 * stored row; the six-color orientation test pins all six faces.
 * Equirect: u = atan2(z,x)/2pi + 0.5, v = acos(y)/pi (row 0 = +Y).
 * Output is linear HDR (no tonemap, no gamma). */

layout(set = 0, binding = 0) uniform texture2D eqMap;
layout(set = 0, binding = 1) uniform sampler envSamp;

layout(push_constant) uniform EqPush {
    int face;
    float roughness; /* unused here; shared push shape */
} push;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;

vec3 faceDir(vec2 fuv, int face) {
    float u = fuv.x;
    float v = fuv.y;
    if (face == 0) {
        return normalize(vec3(1.0, 1.0 - 2.0 * v, 1.0 - 2.0 * u));
    } else if (face == 1) {
        return normalize(vec3(-1.0, 1.0 - 2.0 * v, 2.0 * u - 1.0));
    } else if (face == 2) {
        return normalize(vec3(2.0 * u - 1.0, 1.0, 2.0 * v - 1.0));
    } else if (face == 3) {
        return normalize(vec3(2.0 * u - 1.0, -1.0, 1.0 - 2.0 * v));
    } else if (face == 4) {
        return normalize(vec3(2.0 * u - 1.0, 1.0 - 2.0 * v, 1.0));
    } else {
        return normalize(vec3(1.0 - 2.0 * u, 1.0 - 2.0 * v, -1.0));
    }
}

void main() {
    vec3 dir = faceDir(vUV, push.face);
    vec2 eqUV = vec2(atan(dir.z, dir.x) / TAU + 0.5,
                     acos(clamp(dir.y, -1.0, 1.0)) / PI);
    vec3 c = texture(sampler2D(eqMap, envSamp), eqUV).rgb;
    outColor = vec4(c, 1.0);
}
