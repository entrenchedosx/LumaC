#version 450

/* Luma shadow skinned depth vertex shader (Phase 29): rigid twin
 * of shadow.vert plus linear-blend skinning. The depth layout
 * carries the per-pass light VP at set 0 / binding 0; the skin
 * arena rides set 0 / binding 1 (a second slot on the SAME
 * depth-layout family would break the rigid depth pipeline's
 * 1-slot signature, so skinned depth uses its OWN 2-slot layout
 * + mini-cache — see lr_renderer_skinned_depth_pipeline_for).
 * Push carries model + the skin window (72B). Zero skinJoints
 * takes the rigid path. */

layout(set = 0, binding = 0) uniform ShadowPass {
    mat4 lightVP;
};

layout(set = 0, binding = 1) readonly buffer Skin {
    mat4 joints[];
};

layout(push_constant) uniform Push {
    mat4 model;
    uint skinOffset;
    uint skinJoints;
};

layout(location = 0) in vec3 inPosition;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

void main() {
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
    gl_Position = lightVP * model * vec4(localPos, 1.0);
}
