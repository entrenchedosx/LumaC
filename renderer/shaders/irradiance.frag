#version 450

/* Diffuse irradiance convolution (Phase 17): cosine-weighted
 * hemisphere integral over the base environment cube, fixed
 * LR_ENV_IRRADIANCE_TAPS deterministic taps. Output holds
 * irradiance E(N) = pi * mean(L * cos); the PBR shader divides
 * by pi with albedo. Linear HDR, no tonemap. */

layout(set = 0, binding = 0) uniform textureCube envCube;
layout(set = 0, binding = 1) uniform sampler envSamp;

layout(push_constant) uniform IrrPush {
    int face;
    float roughness; /* unused here; shared push shape */
} push;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979323846;
const uint TAPS = 128u;

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

/* Van der Corput radical inverse, base 2. */
float radicalInverse(uint bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radicalInverse(i));
}

vec3 cosineSample(vec2 xi, vec3 n) {
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt(max(1.0 - xi.y, 0.0));
    float sinT = sqrt(max(xi.y, 0.0));
    vec3 h = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

void main() {
    vec3 n = faceDir(vUV, push.face);
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < TAPS; i++) {
        vec2 xi = hammersley(i, TAPS);
        vec3 l = cosineSample(xi, n);
        float cosT = max(dot(n, l), 0.0);
        sum += texture(samplerCube(envCube, envSamp), l).rgb * cosT;
    }
    outColor = vec4(sum * (PI / float(TAPS)), 1.0);
}
