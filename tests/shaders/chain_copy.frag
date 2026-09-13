#version 450

/* Sample-and-write-through (Phase 19 sync stress): output equals
 * the sampled texel, so chained passes prove write->read
 * visibility pixel-exactly. */

layout(set = 0, binding = 0) uniform texture2D srcTex;
layout(set = 0, binding = 1) uniform sampler srcSamp;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(srcTex, srcSamp), fragUV);
}
