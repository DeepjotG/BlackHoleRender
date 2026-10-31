#version 460 core
// One instance per splat written by gas_splat.comp: a screen-aligned quad around the image.
struct Splat { vec4 a, b, c; };
layout(std430, binding = 13) readonly buffer Splats { Splat sIn[]; };
uniform vec2 uResolution;
out vec2 vOff;
flat out vec4 vInv;
flat out vec3 vCol;
void main() {
    Splat s = sIn[gl_InstanceID];
    vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    vOff = corner * s.a.zw;
    gl_Position = vec4((s.a.xy + vOff) / uResolution * 2.0 - 1.0, 0.0, 1.0);
    vInv = s.b;
    vCol = s.c.rgb;
}
