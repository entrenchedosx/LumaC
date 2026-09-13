#version 450

/* Luma Renderer unlit vertex shader: standard lr_vertex inputs,
 * camera UBO (set 0, binding 0), push model + color. Normal/tangent
 * ride along for the PBR-ready layout (unused by unlit lighting). */

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
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
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
