#version 450

/* Editor environment sky fragment shader (R-012): world-direction
 * gradient in linear HDR (tonemapping happens in the output pass,
 * never here). Three zones over the view-ray elevation:
 *
 *   +Y (zenith)    zoneA — dark blue-slate
 *   ~0 (horizon)   zoneB — slightly brighter desaturated band
 *   -Y (nadir)     zoneC — dark neutral slate
 *
 * The horizon band softness rides zoneC.w. Below-horizon blends to
 * the nadir color so the lower hemisphere reads as ground haze, not
 * a mirror of the sky. No textures, no lights, no IBL contribution:
 * visual background only (never illuminates the scene). */

layout(push_constant) uniform EditorSkyPush {
    mat4 invVP;
    vec4 camPos;
    vec4 zoneA; /* xyz = zenith color, w = unused */
    vec4 zoneB; /* xyz = horizon color, w = unused */
    vec4 zoneC; /* xyz = nadir color, w = horizon softness */
} push;

layout(location = 0) in vec3 vRay;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 d = normalize(vRay);
    float soft = clamp(push.zoneC.w, 0.02, 0.6);
    /* Above horizon: zenith -> horizon band. Below: horizon -> nadir. */
    float up = smoothstep(0.0, soft, d.y);
    float dn = smoothstep(0.0, -soft * 2.0, d.y);
    vec3 col = mix(push.zoneB.xyz, push.zoneA.xyz, up);
    col = mix(col, push.zoneC.xyz, dn);
    outColor = vec4(col, 1.0);
}
