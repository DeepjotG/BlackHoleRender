#version 460 core
// Final pass: bloom, chromatic aberration, exposure, tonemap, vignette, grain, sRGB encode.
#include "common.glsl"

in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform sampler2D uHdr;
uniform sampler2D uBloom;
uniform vec2  uResolution;
uniform float uExposure;
uniform int   uTonemap;
uniform float uVignette;
uniform float uGrain;
uniform float uSaturation;
uniform float uBloomIntensity;
uniform float uChroma;

// ---- AgX (Troy Sobotka), minimal fit by Benjamin Wrensch ----
vec3 agxDefaultContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 agx(vec3 val) {
    const mat3 agx_mat = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                              0.0784335999999992, 0.878468636469772, 0.0784336,
                              0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    val = agx_mat * val;
    val = clamp(log2(max(val, 1e-10)), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    return agxDefaultContrastApprox(val);
}
vec3 agxEotf(vec3 val) {
    const mat3 agx_mat_inv = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                  -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                  -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    val = agx_mat_inv * val;
    return pow(max(val, 0.0), vec3(2.2));
}
vec3 acesFitted(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 toSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 fetchScene(vec2 uv) {
    return texture(uHdr, uv).rgb + texture(uBloom, uv).rgb * uBloomIntensity;
}

void main() {
    vec2 q = vUv * 2.0 - 1.0;
    vec3 hdr;
    if (uChroma > 0.001) {
        vec2 off = q * (uChroma * 0.004) * length(q);
        hdr.r = fetchScene(vUv + off).r;
        hdr.g = fetchScene(vUv).g;
        hdr.b = fetchScene(vUv - off).b;
    } else {
        hdr = fetchScene(vUv);
    }
    hdr *= uExposure;
    float l = luminance(hdr);
    hdr = max(mix(vec3(l), hdr, uSaturation), 0.0);

    vec3 c;
    if (uTonemap == 0) c = agxEotf(agx(hdr));
    else if (uTonemap == 1) c = acesFitted(hdr);
    else if (uTonemap == 2) c = hdr / (1.0 + hdr);
    else c = hdr;

    vec2 qa = q;
    qa.x *= uResolution.x / uResolution.y;
    float vig = 1.0 - uVignette * smoothstep(0.6, 2.2, length(qa) * 0.9);
    c *= vig;

    float g = hash12(gl_FragCoord.xy + fract(uTime * 7.31) * 1024.0) - 0.5;
    c += g * uGrain * (0.15 + 0.85 * (1.0 - luminance(c)));

    fragColor = vec4(toSrgb(c), 1.0);
}
