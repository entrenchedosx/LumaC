#version 450

/* Phase 11 cube vertex shader: indexed position + UV plus a
 * per-instance offset, transformed by a push-constant MVP. */

layout(push_constant) uniform Push {
    mat4 mvp;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inInstanceOffset;

layout(location = 0) out vec2 fragUV;

void main()
{
    vec3 world = inPosition + inInstanceOffset;
    gl_Position = mvp * vec4(world, 1.0);
    fragUV = inUV;
}
