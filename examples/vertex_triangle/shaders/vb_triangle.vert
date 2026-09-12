#version 450

/* Vertex-buffer triangle: position and color arrive as vertex inputs
 * (locations 0 and 1). Same triangle as the index-generated example,
 * proving the real resource path renders identically. */

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main()
{
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
