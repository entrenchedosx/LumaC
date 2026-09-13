#version 450

/* Luma PBR fragment shader (Phase 15 direct + Phase 17 IBL):
 * metallic-roughness Cook-Torrance (GGX + Schlick-GGX Smith +
 * Schlick Fresnel + Lambert) plus split-sum image-based lighting
 * (irradiance cubemap + GGX prefilter chain + BRDF LUT). No
 * tonemapping or transparency — the output pass handles those;
 * see docs/PBR_ARCHITECTURE.md and docs/ENVIRONMENT_ARCHITECTURE.md
 * for the exact equations and the deliberate approximations
 * (ambient fallback without env, occlusion-on-indirect, single
 * shared sampler).
 *
 * Conventions: direction vectors are light TRAVEL directions
 * (L = -direction). metallic = factor * texel.B, roughness =
 * factor * texel.G, occlusion = texel.R. Base-color/emissive maps
 * are sRGB (decoded by the GPU format); data maps are linear. */

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 camPos;
};

struct Light {
    vec4 colorIntensity; /* rgb + intensity */
    vec4 posRange;       /* xyz position, w range */
    vec4 dirKind;        /* xyz travel direction, w kind (0/1/2) */
    vec4 spotAngles;     /* cosInner, cosOuter, 0, 0 */
    vec4 shadowInfo;     /* enabled01, slot(-1 none), 0, 0 */
};

layout(set = 0, binding = 1) uniform Lights {
    vec4 ambient;
    uvec4 counts; /* x = active light count */
    Light lights[64];
};

/* Frame shadow state (renderer-owned set, slot 1): metadata plus
 * four fixed map slots and one shared sampler. Slot selection uses
 * static branches over constant indices (no dynamic indexing
 * anywhere). Inactive slots bind the white fallback (value 1 = lit).
 * Mirrors lr_shadows_gpu exactly (slot count, pad, 4x {mat4, vec4}). */
struct ShadowSlot {
    mat4 viewProj;
    vec4 params; /* texel, normalBias, constBias, 0 */
};

layout(set = 1, binding = 0) uniform ShadowMeta {
    uvec4 slotCount; /* x = assigned slots */
    ShadowSlot slots[4];
};

layout(set = 1, binding = 1) uniform texture2D shadowMap0;
layout(set = 1, binding = 2) uniform texture2D shadowMap1;
layout(set = 1, binding = 3) uniform texture2D shadowMap2;
layout(set = 1, binding = 4) uniform texture2D shadowMap3;
layout(set = 1, binding = 5) uniform sampler shadowSamp;

/* Frame environment state (renderer-owned set, slot 2, PBR only):
 * irradiance + prefiltered specular cubemaps, the shared BRDF
 * integration LUT, one sampler, and runtime parameters. The set is
 * always bound (an empty black environment when none is active),
 * so shaders never branch on set presence — only on w. */
layout(set = 2, binding = 0) uniform EnvParams {
    vec4 envPrm; /* x = intensity, y = yaw rotation, z = prefilter
                    mip count, w = IBL active 0/1 */
};

layout(set = 2, binding = 1) uniform textureCube irrMap;
layout(set = 2, binding = 2) uniform textureCube prefMap;
layout(set = 2, binding = 3) uniform texture2D brdfMap;
layout(set = 2, binding = 4) uniform sampler envSamp;

layout(push_constant) uniform Push {
    mat4 model;
    mat3 normalMat;
    uint drawFlags; /* bit0: receives shadows */
};

/* 3x3 PCF over one slot. Returns 1 for anything outside the shadow
 * projection (lit, never clamped to arbitrary depth), including
 * behind-light geometry (w <= 0). Receiver depth carries the
 * constant bias; the sample position carries the normal bias
 * (applied by the caller along the geometric normal, never the
 * tangent-space normal). */
float shadow_pcf(int slot, vec3 worldPos, vec3 geoN) {
    mat4 vp;
    vec4 prm;
    vec3 opos;
    vec4 sc;
    vec3 proj;
    vec2 uv;
    float recv;
    float sum = 0.0;

    if (slot == 0) {
        vp = slots[0].viewProj;
        prm = slots[0].params;
    } else if (slot == 1) {
        vp = slots[1].viewProj;
        prm = slots[1].params;
    } else if (slot == 2) {
        vp = slots[2].viewProj;
        prm = slots[2].params;
    } else {
        vp = slots[3].viewProj;
        prm = slots[3].params;
    }
    opos = worldPos + geoN * prm.y;
    sc = vp * vec4(opos, 1.0);
    if (sc.w <= 0.0) {
        return 1.0;
    }
    proj = sc.xyz / sc.w;
    uv = proj.xy * 0.5 + vec2(0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0) {
        return 1.0;
    }
    recv = proj.z - prm.z;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored;

            if (slot == 0) {
                stored = texture(sampler2D(shadowMap0, shadowSamp),
                                 uv + vec2(float(x), float(y)) * prm.x).x;
            } else if (slot == 1) {
                stored = texture(sampler2D(shadowMap1, shadowSamp),
                                 uv + vec2(float(x), float(y)) * prm.x).x;
            } else if (slot == 2) {
                stored = texture(sampler2D(shadowMap2, shadowSamp),
                                 uv + vec2(float(x), float(y)) * prm.x).x;
            } else {
                stored = texture(sampler2D(shadowMap3, shadowSamp),
                                 uv + vec2(float(x), float(y)) * prm.x).x;
            }
            sum += step(recv, stored);
        }
    }
    return sum / 9.0;
}

layout(set = 0, binding = 2) uniform Material {
    vec4 baseColor;
    vec3 emissive;
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    uint flags;
    vec3 pad; /* 64 bytes total; mirrors lr_material_gpu */
};

layout(set = 0, binding = 3) uniform texture2D baseColorTex;
layout(set = 0, binding = 4) uniform texture2D metallicRoughnessTex;
layout(set = 0, binding = 5) uniform texture2D normalTex;
layout(set = 0, binding = 6) uniform texture2D occlusionTex;
layout(set = 0, binding = 7) uniform texture2D emissiveTex;
layout(set = 0, binding = 8) uniform sampler matSampler;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec4 inWorldTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outColor;

#define LR_PI 3.14159265358979323846
#define LR_MIN_ROUGHNESS 0.05
#define LR_DIELECTRIC_F0 0.04

void main() {
    vec4 baseTexel = texture(sampler2D(baseColorTex, matSampler), inUV);
    vec3 albedo = baseColor.rgb * baseTexel.rgb;
    float baseAlpha = baseColor.a * baseTexel.a;

    vec4 mrTexel = texture(sampler2D(metallicRoughnessTex, matSampler), inUV);
    float metal = clamp(metallic * mrTexel.b, 0.0, 1.0);
    float rough = clamp(roughness * mrTexel.g,
                        LR_MIN_ROUGHNESS, 1.0);

    /* Tangent basis with handedness; backfaces flip the shading
     * normal (double-sided variant disables culling). geoN is the
     * geometric normal (shadow offsets use it, never the
     * tangent-space normal). */
    vec3 n = normalize(inWorldNormal);
    vec3 t = normalize(inWorldTangent.xyz);
    if (!gl_FrontFacing) {
        n = -n;
    }
    vec3 geoN = n;
    vec3 b = inWorldTangent.w * cross(n, t);
    vec3 mapN = texture(sampler2D(normalTex, matSampler), inUV).xyz * 2.0 - 1.0;
    mapN.xy *= normalScale;
    n = normalize(t * mapN.x + b * mapN.y + n * mapN.z);

    vec3 v = normalize(camPos.xyz - inWorldPos);
    float nDotV = max(dot(n, v), 0.0);
    vec3 f0 = mix(vec3(LR_DIELECTRIC_F0), albedo, metal);

    float alpha = rough * rough;
    float alpha2 = alpha * alpha;
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;

    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < 64u; ++i) {
        vec3 l;
        float attenuation;
        float shadow = 1.0;

        if (i >= counts.x) {
            break;
        }
        if (lights[i].dirKind.w < 0.5) {
            l = -lights[i].dirKind.xyz;
            attenuation = 1.0;
        } else if (lights[i].dirKind.w < 1.5) {
            vec3 toLight = lights[i].posRange.xyz - inWorldPos;
            float dist = length(toLight);
            float range = max(lights[i].posRange.w, 1e-4);
            float x = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
            l = toLight / max(dist, 1e-4);
            attenuation = (x * x) / (dist * dist + 1.0);
        } else {
            /* Spot: point-style range falloff times the cone. */
            vec3 toLight = lights[i].posRange.xyz - inWorldPos;
            float dist = length(toLight);
            float range = max(lights[i].posRange.w, 1e-4);
            float x = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
            float cosTheta;
            float fall;

            l = toLight / max(dist, 1e-4);
            attenuation = (x * x) / (dist * dist + 1.0);
            cosTheta = dot(-l, lights[i].dirKind.xyz);
            if (lights[i].spotAngles.x > lights[i].spotAngles.y) {
                fall = smoothstep(lights[i].spotAngles.y,
                                  lights[i].spotAngles.x, cosTheta);
            } else {
                /* Degenerate equal cone: hard step (smoothstep with
                 * equal edges is undefined). */
                fall = step(lights[i].spotAngles.y, cosTheta);
            }
            attenuation *= fall;
        }
        float nDotL = max(dot(n, l), 0.0);
        if (nDotL <= 0.0) {
            continue;
        }
        /* Shadow gates direct light only (ambient/emissive never).
         * Unshadowed lights and non-receiving draws stay at 1. */
        if (drawFlags != 0u && lights[i].shadowInfo.x > 0.5) {
            shadow = shadow_pcf(int(lights[i].shadowInfo.y + 0.5),
                                inWorldPos, geoN);
        }
        vec3 h = normalize(l + v);
        float nDotH = max(dot(n, h), 0.0);
        float vDotH = max(dot(v, h), 0.0);

        float denom = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
        float d = alpha2 / (LR_PI * denom * denom);
        float g1v = nDotV / (nDotV * (1.0 - k) + k);
        float g1l = nDotL / (nDotL * (1.0 - k) + k);
        float g = g1v * g1l;
        vec3 f = f0 + (vec3(1.0) - f0) * pow(1.0 - vDotH, 5.0);
        vec3 spec = (d * g * f) / max(4.0 * nDotV * nDotL, 1e-4);
        vec3 kD = (vec3(1.0) - f) * (1.0 - metal);
        vec3 radiance = lights[i].colorIntensity.rgb * lights[i].colorIntensity.a;
        direct += (kD * albedo / LR_PI + spec) * (nDotL * attenuation * shadow) * radiance;
    }

    /* Occlusion damps indirect light (ambient fallback without an
     * environment, IBL diffuse fully + specular at a documented
     * half rate with one); direct lighting is untouched by design. */
    float occ = mix(1.0,
                    texture(sampler2D(occlusionTex, matSampler), inUV).r,
                    occlusionStrength);
    vec3 emissiveTerm =
        emissive * texture(sampler2D(emissiveTex, matSampler), inUV).rgb;
    /* Split-sum IBL (uniform branch; skipped entirely without an
     * environment so the legacy path below stays bit-identical).
     * Rotation turns sample directions for lighting and sky
     * together. Irradiance holds E(N); diffuse = albedo/pi * E.
     * Specular = prefilteredEnv(roughness-mapped LOD) * (F0*A+B). */
    vec3 indirect = vec3(0.0);
    if (envPrm.w > 0.5) {
        float cy = cos(envPrm.y);
        float sy = sin(envPrm.y);
        vec3 rN = vec3(cy * n.x + sy * n.z, n.y, -sy * n.x + cy * n.z);
        vec3 refl = reflect(-v, n);
        vec3 rR = vec3(cy * refl.x + sy * refl.z, refl.y,
                       -sy * refl.x + cy * refl.z);
        vec3 irr = texture(samplerCube(irrMap, envSamp), rN).rgb;
        float lodMax = max(envPrm.z - 1.0, 0.0);
        vec3 pre = textureLod(samplerCube(prefMap, envSamp), rR,
                              clamp(rough * lodMax, 0.0, lodMax)).rgb;
        vec2 envBRDF = texture(sampler2D(brdfMap, envSamp),
                               vec2(clamp(nDotV, 0.0, 1.0), rough)).rg;
        vec3 diffIBL = irr * albedo * (1.0 - metal) / LR_PI;
        vec3 specIBL = pre * (f0 * envBRDF.x + envBRDF.y);
        indirect = (diffIBL * occ + specIBL * mix(1.0, occ, 0.5)) *
                   envPrm.x;
    }
    vec3 color = direct +
                 albedo * (1.0 - metal) * ambient.rgb * occ *
                     (1.0 - envPrm.w) +
                 indirect + emissiveTerm;
    outColor = vec4(color, baseAlpha);
}
