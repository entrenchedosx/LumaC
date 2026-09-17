#version 450

/* Phase 33 editor GUI vertex shader (Dear ImGui draw walk).
 *
 * Input: ImDrawVert (location 0 = pos R32G32_FLOAT, location 1 = uv
 * R32G32_FLOAT, location 2 = col R8G8B8A8_UNORM). Push constants carry
 * the projection (scale.xy, translate.xy) exactly like the upstream
 * Dear ImGui Vulkan backend:
 *   scale     = 2 / DisplaySize
 *   translate = -1 - DisplayPos * scale
 * Y points down (Vulkan framebuffer convention); no viewport flip.
 */

layout(push_constant) uniform uPushConstant {
    vec2 uScale;
    vec2 uTranslate;
} pc;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main() {
    vColor = aColor;
    vUV = aUV;
    gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0.0, 1.0);
}
