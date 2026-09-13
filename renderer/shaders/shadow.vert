#version 450

/* Luma shadow depth vertex shader (Phase 16): light-space position
 * only. No fragment stage (LumaC fragment-optional pipelines):
 * rasterization writes depth, nothing else. One shared pipeline
 * with cull NONE covers all casters (documented rule — never the
 * main-pass culling state). */

layout(set = 0, binding = 0) uniform ShadowPass {
    mat4 lightVP;
};

layout(push_constant) uniform Push {
    mat4 model;
};

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = lightVP * model * vec4(inPosition, 1.0);
}
