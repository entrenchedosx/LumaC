#version 450

/* Textured quad fragment shader: separate texture and sampler
 * bindings combined in-shader (matches LumaC's distinct
 * sampled-image / sampler binding model). */

layout(set = 0, binding = 1) uniform texture2D tex;
layout(set = 0, binding = 2) uniform sampler samp;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(tex, samp), fragUV);
}
