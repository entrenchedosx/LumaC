#version 450

/* Luma Renderer unlit fragment shader: base-color texture modulated
 * by the push-constant material color. */

layout(set = 0, binding = 1) uniform texture2D baseTex;
layout(set = 0, binding = 2) uniform sampler baseSamp;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
};

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(baseTex, baseSamp), fragUV) * fragColor;
}
