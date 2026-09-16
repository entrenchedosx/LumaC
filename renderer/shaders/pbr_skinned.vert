#version 450

/* Luma PBR skinned vertex shader (Phase 29): rigid twin of
 * pbr.vert plus linear-blend skinning over the shared lr_vertex
 * stride. Joints/weights ride locations 4/5 (uvec4/vec4); the
 * frame skin arena rides set 3 / binding 0 as an array of
 * column-major mat4s (std430, 16 floats each, matching the CPU
 * arena layout exactly). Per-draw skin_offset/skin_joints ride
 * the push block (124B total, under the 128B limit); both zero
 * takes the rigid path (no arena read, byte-identical to
 * pbr.vert). See docs/GPU_SKINNING.md for the frame
 * reconciliation, weight policy, and OOB clamp rules. */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

layout(set = 3, binding = 0) readonly buffer Skin {
    mat4 joints[];
};

layout(push_constant) uniform Push {
    mat4 model;
    mat3 normalMat;
    uint drawFlags;
    uint skinOffset;
    uint skinJoints;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec4 outWorldTangent;
layout(location = 3) out vec2 outUV;

void main() {
    vec3 localPos = inPosition;
    vec3 localNormal = inNormal;
    vec3 localTangent = inTangent.xyz;
    if (skinJoints > 0u) {
        /* Linear-blend skinning in the mesh local frame. Weights
         * normalize (divide by sum); a zero-weight vertex holds
         * its bind position (never NaN). Joint indices clamp to
         * [0, skinJoints - 1] (OOB-safe by construction). */
        float wsum = inWeights[0] + inWeights[1] + inWeights[2] +
                     inWeights[3];
        if (wsum > 1e-9) {
            vec3 p = vec3(0.0);
            vec3 n = vec3(0.0);
            vec3 t = vec3(0.0);
            for (int k = 0; k < 4; k++) {
                uint j = inJoints[k];
                if (j >= skinJoints) {
                    j = skinJoints - 1u;
                }
                mat4 m = joints[skinOffset + j];
                float w = inWeights[k] / wsum;
                p += w * (m * vec4(inPosition, 1.0)).xyz;
                n += w * mat3(m) * inNormal;
                t += w * mat3(m) * inTangent.xyz;
            }
            localPos = p;
            localNormal = n;
            localTangent = t;
        }
    }
    vec4 worldPos = model * vec4(localPos, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = normalMat * localNormal;
    outWorldTangent = vec4(normalMat * localTangent, inTangent.w);
    outUV = inUV;
    gl_Position = viewProj * worldPos;
}
