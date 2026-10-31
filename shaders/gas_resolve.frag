#version 460 core
// Lays the composited gas splats over the HDR image (blend ONE, SRC_ALPHA): the gas's average colour
// times its coverage, and the image behind dimmed by the gas's transmittance.
in vec2 vUv;
uniform sampler2D uAccum;
uniform sampler2D uReveal;
out vec4 fragColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 acc = texelFetch(uAccum, p, 0);
    if (acc.a <= 1e-7) discard;
    float reveal = texelFetch(uReveal, p, 0).r;
    fragColor = vec4(acc.rgb / acc.a * (1.0 - reveal), reveal);
}
