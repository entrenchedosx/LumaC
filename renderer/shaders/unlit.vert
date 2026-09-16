#version 450

/* Luma Renderer unlit vertex shader: position + UV over the
 * shared lr_vertex stride, camera UBO (set 0, binding 0), push model
 * + color. Normal/tangent stay in the stride for the PBR layout but
 * are NOT declared here: declaring unconsumed inputs trips
 * "attribute not consumed" validation noise (Stage 83 audit), so
 * the unlit pipeline binds only locations 0 and 3 by design. */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
};

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main()
{
    vec4 world = model * vec4(inPosition, 1.0);
    gl_Position = viewProj * world;
    fragUV = inUV;
    fragColor = color;
}
