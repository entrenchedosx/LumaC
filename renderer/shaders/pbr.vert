#version 450

/* Luma PBR vertex shader (Phase 15): world-space interpolants for
 * the Cook-Torrance fragment stage. Normals/tangents arrive via the
 * CPU-side normal matrix (inverse-transpose), never the raw model
 * matrix, so non-uniform scale shades correctly. */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

layout(push_constant) uniform Push {
    mat4 model;
    mat3 normalMat;
    uint drawFlags;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec4 outWorldTangent;
layout(location = 3) out vec2 outUV;

void main() {
    vec4 worldPos = model * vec4(inPosition, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = normalMat * inNormal;
    outWorldTangent = vec4(normalMat * inTangent.xyz, inTangent.w);
    outUV = inUV;
    gl_Position = viewProj * worldPos;
}
