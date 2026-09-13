#version 450

/* MRT constant outputs: attachment 0 solid red, attachment 1 solid
 * green. Proves distinct per-attachment values. */

layout(location = 0) out vec4 outColor0;
layout(location = 1) out vec4 outColor1;

void main()
{
    outColor0 = vec4(1.0, 0.0, 0.0, 1.0);
    outColor1 = vec4(0.0, 1.0, 0.0, 1.0);
}
