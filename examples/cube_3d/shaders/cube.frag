#version 450

/* Phase 11 cube fragment shader: separate texture/sampler bindings
 * combined in-shader (matches LumaC's distinct sampled-image /
 * sampler model). */

layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(tex, samp), fragUV);
}
