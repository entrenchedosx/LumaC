#version 450

/* Split-sum BRDF integration LUT (Phase 17, Epic 2013 formulation):
 * x = NdotV, y = roughness; outputs (scale, bias) so specular IBL
 * = prefilteredEnv * (F0 * A + B). Environment-independent: built
 * once per renderer, never per frame or per environment.
 * Fixed LR_ENV_BRDF_TAPS deterministic taps. Linear data. */

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979323846;
const uint TAPS = 128u;

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

float geometrySmith(float nDotV, float nDotL, float a) {
    float k = (a + 1.0) * (a + 1.0) / 8.0;
    float g1v = nDotV / (nDotV * (1.0 - k) + k);
    float g1l = nDotL / (nDotL * (1.0 - k) + k);
    return g1v * g1l;
}

vec2 integrateBRDF(float nDotV, float roughness) {
    vec3 v = vec3(sqrt(max(1.0 - nDotV * nDotV, 0.0)), 0.0, nDotV);
    vec3 n = vec3(0.0, 0.0, 1.0);
    float a = max(roughness * roughness, 0.0025);
    float A = 0.0;
    float B = 0.0;
    for (uint i = 0u; i < TAPS; i++) {
        vec2 xi = hammersley(i, TAPS);
        vec3 h = ggxSample(xi, n, a);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);
        float nDotL = max(l.z, 0.0);
        float nDotH = max(h.z, 0.0);
        float vDotH = max(dot(v, h), 0.0);
        if (nDotL > 0.0) {
            float g = geometrySmith(nDotV, nDotL, a);
            float gv = g * vDotH / max(nDotH * nDotV, 1e-4);
            float fc = pow(1.0 - vDotH, 5.0);
            A += (1.0 - fc) * gv;
            B += fc * gv;
        }
    }
    return vec2(A, B) / float(TAPS);
}

void main() {
    vec2 brdf = integrateBRDF(clamp(vUV.x, 0.0, 1.0), clamp(vUV.y, 0.0, 1.0));
    outColor = vec4(brdf, 0.0, 1.0);
}
