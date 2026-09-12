#version 450

/* Textured quad vertex shader: position + UV from vertex buffers,
 * transformed by a uniform matrix. No vertex-index generation. */

layout(set = 0, binding = 0) uniform Transform {
    mat4 mvp;
};

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main()
{
    gl_Position = mvp * vec4(inPosition, 0.0, 1.0);
    fragUV = inUV;
}
