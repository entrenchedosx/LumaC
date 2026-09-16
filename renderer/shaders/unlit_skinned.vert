#version 450

/* Luma Renderer unlit skinned vertex shader (Phase 29): rigid
 * twin of unlit.vert plus linear-blend skinning over the shared
 * lr_vertex stride. Only position + UV vary per vertex on the
 * rigid path (same Stage-83 rule as unlit.vert: never declare
 * unconsumed attributes), plus the skin pair at locations 4/5.
 * The frame skin arena rides set 1 / binding 0 (unlit slot 0
 * stays the material set); the push block carries model + color
 * + the skin window (88B). Zero skinJoints takes the rigid path
 * (byte-identical to unlit.vert). */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

layout(set = 1, binding = 0) readonly buffer Skin {
    mat4 joints[];
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    uint skinOffset;
    uint skinJoints;
};

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inUV;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main()
{
    vec3 localPos = inPosition;
    if (skinJoints > 0u) {
        float wsum = inWeights[0] + inWeights[1] + inWeights[2] +
                     inWeights[3];
        if (wsum > 1e-9) {
            vec3 p = vec3(0.0);
            for (int k = 0; k < 4; k++) {
                uint j = inJoints[k];
                if (j >= skinJoints) {
                    j = skinJoints - 1u;
                }
                float w = inWeights[k] / wsum;
                p += w * (joints[skinOffset + j] *
                          vec4(inPosition, 1.0)).xyz;
            }
            localPos = p;
        }
    }
    vec4 world = model * vec4(localPos, 1.0);
    gl_Position = viewProj * world;
    fragUV = inUV;
    fragColor = color;
}
