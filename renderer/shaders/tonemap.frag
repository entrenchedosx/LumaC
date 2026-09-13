#version 450

/* Tonemap output pass (Phase 17): exposure + operator on the HDR
 * scene, then clamp into LDR range. NONE clamps (never lets
 * inf/NaN/negatives reach UNORM); ACES is the Narkowicz fitted
 * filmic curve on exposure-scaled input. No gamma here: sRGB
 * encoding happens exactly once via the output target format. */

layout(set = 0, binding = 0) uniform texture2D hdrTex;
layout(set = 0, binding = 1) uniform sampler hdrSamp;

layout(push_constant) uniform TmPush {
    float evMult; /* 2^EV, applied pre-tonemap */
    int op;       /* 0 = NONE, 1 = ACES */
} push;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

void main() {
    vec3 v = texture(sampler2D(hdrTex, hdrSamp), vUV).rgb * push.evMult;
    /* NaN can never reach UNORM output (clamp alone is undefined
     * for NaN); negatives and infinities clamp below. */
    v = mix(v, vec3(0.0), isnan(v));
    if (push.op == 1) {
        v = aces(max(v, vec3(0.0)));
    }
    outColor = vec4(clamp(v, vec3(0.0), vec3(1.0)), 1.0);
}
