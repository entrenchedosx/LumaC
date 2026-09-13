#version 450

/* Procedural 4x4 checkerboard (no textures): pass-1 source for the
 * two-pass pixel test. Exact constant outputs, no filtering. */

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    int cx = int(floor(fragUV.x * 4.0));
    int cy = int(floor(fragUV.y * 4.0));
    int white = (cx + cy) % 2;

    /* Clamp overscan (fullscreen-triangle UVs reach 2.0). */
    if (fragUV.x > 1.0 || fragUV.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    outColor = (white == 0) ? vec4(1.0) : vec4(0.05, 0.05, 0.05, 1.0);
}
