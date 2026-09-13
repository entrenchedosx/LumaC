#version 450

/* Tint verification post effect (Phase 18, PART AE): color-multiply
 * the chain input by a runtime tint. Identity (1,1,1) by default so
 * enabling the stage with no tint change is a byte-exact no-op —
 * the test then applies a non-identity tint to prove scene ->
 * intermediate -> post -> tonemap/output chaining. No gamma here. */

layout(set = 0, binding = 0) uniform texture2D postTex;
layout(set = 0, binding = 1) uniform sampler postSamp;

layout(push_constant) uniform TintPush {
    vec3 tint; /* color multiplier */
} push;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 v = texture(sampler2D(postTex, postSamp), vUV).rgb;
    outColor = vec4(v * push.tint, 1.0);
}
