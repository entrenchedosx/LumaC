#version 450

/* GGX prefiltered specular convolution (Phase 17): importance-
 * sampled environment for one roughness level (push.roughness in
 * [0,1]). Fixed LR_ENV_PREFILTER_TAPS deterministic taps on mip 0;
 * blur converges by integration (no mip-bias shortcut), so roughness
 * 0 stays sharp and 1 fully diffuse. Linear HDR, no tonemap. */

layout(set = 0, binding = 0) uniform textureCube envCube;
layout(set = 0, binding = 1) uniform sampler envSamp;

layout(push_constant) uniform PrefPush {
    int face;
    float roughness;
} push;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979323846;
const uint TAPS = 256u;

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

vec3 ggxSample(vec2 xi, vec3 n, float a) {
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt(max((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y), 0.0));
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    vec3 h = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

void main() {
    vec3 n = faceDir(vUV, push.face);
    vec3 v = n;
    float rough = clamp(push.roughness, 0.0, 1.0);
    float a = max(rough * rough, 0.0025);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < TAPS; i++) {
        vec2 xi = hammersley(i, TAPS);
        vec3 h = ggxSample(xi, n, a);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndl = max(dot(n, l), 0.0);
        if (ndl > 0.0) {
            sum += textureLod(samplerCube(envCube, envSamp), l, 0.0).rgb * ndl;
            wsum += ndl;
        }
    }
    outColor = vec4(sum / max(wsum, 1e-4), 1.0);
}
