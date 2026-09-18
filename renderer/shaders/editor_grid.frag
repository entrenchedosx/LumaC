#version 450

/* Editor infinite grid fragment shader (R-012): procedural world-space
 * grid on the y=0 plane, drawn as a fullscreen triangle AFTER scene
 * geometry with depth test ON (LESS) and depth writes OFF, so authored
 * content naturally obscures it and it never punches through floors.
 *
 * Technique: reconstruct the world position per pixel from the same
 * invVP + camPos uniforms the sky uses (ray = world - camPos), intersect
 * the ray with the y=0 plane in the shader, derive 1m minor / 10m major
 * cells with fwidth-based anti-aliasing, fade with distance, and tint the
 * X/Z axes subtly near the origin. Discards where the ray misses the plane
 * (above horizon), where the camera is below the plane (clean fade rule:
 * no grid, sky only), or where the fade reaches zero.
 *
 * All tuning rides push constants (no textures, no vertex buffers). */

layout(push_constant) uniform EditorGridPush {
    mat4 invVP;
    vec4 camPos;
    vec4 gridA; /* xyz = minor color, w = minor alpha */
    vec4 gridB; /* xyz = major color, w = major alpha */
    vec4 gridC; /* xyz = axis colors are derived below, w = fade distance */
    vec4 gridD; /* x = axis tint strength, y = axis falloff, z/w unused */
} push;

layout(location = 0) in vec3 vRay;
layout(location = 0) out vec4 outColor;

float gridLine(vec2 p, float scale) {
    vec2 q = abs(fract(p / scale - 0.5) - 0.5) * scale / fwidth(p);
    return 1.0 - min(min(q.x, q.y), 1.0);
}

void main() {
    vec3 ro = push.camPos.xyz;
    vec3 rd = normalize(vRay);
    /* Below-plane camera: no grid (sky only). Grazing rays miss. */
    if (ro.y < 0.0 || abs(rd.y) < 1e-5) {
        discard;
    }
    float t = -ro.y / rd.y;
    if (t <= 0.0) {
        discard;
    }
    vec3 hit = ro + rd * t;
    float dist = length(hit - ro);
    float fadeDist = max(push.gridC.w, 1.0);
    float fade = 1.0 - smoothstep(fadeDist * 0.35, fadeDist, dist);
    if (fade <= 0.001) {
        discard;
    }
    vec2 p = hit.xz;
    float minor = gridLine(p, 1.0);
    float major = gridLine(p, 10.0);
    float alpha = minor * push.gridA.w + major * push.gridB.w;
    vec3 col = (minor * push.gridA.rgb * push.gridA.w +
                major * push.gridB.rgb * push.gridB.w);
    /* Axis emphasis: restrained red on X (z~0), restrained blue on Z
     * (x~0), fading a few metres from the origin. */
    float axW = fwidth(p.y) * 1.5 + 0.03;
    float azW = fwidth(p.x) * 1.5 + 0.03;
    float axLine = 1.0 - smoothstep(0.0, axW, abs(p.y));
    float azLine = 1.0 - smoothstep(0.0, azW, abs(p.x));
    float axFall = exp(-dot(p, p) / (push.gridD.y * push.gridD.y));
    float ax = max(axLine, azLine) * axFall * push.gridD.x;
    vec3 axCol = (axLine > azLine) ? vec3(0.55, 0.23, 0.20)
                                   : vec3(0.23, 0.34, 0.55);
    col = mix(col, axCol, clamp(ax, 0.0, 1.0));
    alpha = max(alpha, ax * 0.6);
    outColor = vec4(col, alpha * fade);
}
