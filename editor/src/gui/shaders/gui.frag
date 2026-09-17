#version 450

/* Phase 33 editor GUI fragment shader (Dear ImGui draw walk).
 *
 * Separate texture/sampler bindings combined in-shader (matches
 * LumaC's distinct sampled-image / sampler binding model and the
 * upstream backend's set-0-texture / set-1-sampler split).
 */

layout(set = 0, binding = 0) uniform texture2D uTexture;
layout(set = 1, binding = 0) uniform sampler uSampler;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor * texture(sampler2D(uTexture, uSampler), vUV);
}
