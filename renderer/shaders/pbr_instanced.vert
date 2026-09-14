#version 450

/* Luma PBR instanced vertex shader (Phase 21): GPU-driven twin of
 * pbr.vert. The model matrix comes from a storage buffer indexed by
 * the compacted visible list (gl_InstanceIndex addresses visible
 * order, not submission order), so one indirect draw renders every
 * visible instance of a mesh/material group. Normals use
 * mat3(model): exact for rigid and uniform-scale transforms, which
 * is all GPU-driven scenes submit (non-uniform shading stays on
 * the CPU per-draw path). Outputs match pbr.vert exactly, so the
 * same fragment stage runs unmodified. */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

struct GpuInstance {
    /* Must match the culling input layout exactly (96-byte stride:
     * mat4 + vec4 + 4x uint). Only .model is read here. */
    mat4 model;
    vec4 bounds;
    uint meshIndex;
    uint objectId;
    uint pad0;
    uint pad1;
};

layout(set = 3, binding = 0) readonly buffer Instances {
    GpuInstance items[];
} instances;

layout(set = 3, binding = 1) readonly buffer Visible {
    uint indices[];
} visible;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec4 outWorldTangent;
layout(location = 3) out vec2 outUV;

void main() {
    uint slot = visible.indices[gl_InstanceIndex];
    mat4 model = instances.items[slot].model;
    mat3 normalMat = mat3(model);
    vec4 worldPos = model * vec4(inPosition, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = normalMat * inNormal;
    outWorldTangent = vec4(normalMat * inTangent.xyz, inTangent.w);
    outUV = inUV;
    gl_Position = viewProj * worldPos;
}
