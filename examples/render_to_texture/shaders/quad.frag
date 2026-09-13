#version 450

/* Second-pass sampling shader: displays an offscreen color target
 * (editor-viewport primitive). */

layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sampler2D(tex, samp), fragUV);
}
