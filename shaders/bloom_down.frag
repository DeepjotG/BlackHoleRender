#version 460 core
// 13-tap downsample (Jimenez 2014). The first level applies a soft-knee threshold.
#include "common.glsl"
in vec2 vUv;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uSrcTexel;
uniform float uThreshold;   // < 0 disables thresholding
void main() {
    vec2 t = uSrcTexel;
    vec3 a = texture(uSrc, vUv + t * vec2(-2.0, -2.0)).rgb;
    vec3 b = texture(uSrc, vUv + t * vec2(0.0, -2.0)).rgb;
    vec3 c = texture(uSrc, vUv + t * vec2(2.0, -2.0)).rgb;
    vec3 d = texture(uSrc, vUv + t * vec2(-2.0, 0.0)).rgb;
    vec3 e = texture(uSrc, vUv).rgb;
    vec3 f = texture(uSrc, vUv + t * vec2(2.0, 0.0)).rgb;
    vec3 g = texture(uSrc, vUv + t * vec2(-2.0, 2.0)).rgb;
    vec3 h = texture(uSrc, vUv + t * vec2(0.0, 2.0)).rgb;
    vec3 i = texture(uSrc, vUv + t * vec2(2.0, 2.0)).rgb;
    vec3 j = texture(uSrc, vUv + t * vec2(-1.0, -1.0)).rgb;
    vec3 k = texture(uSrc, vUv + t * vec2(1.0, -1.0)).rgb;
    vec3 l = texture(uSrc, vUv + t * vec2(-1.0, 1.0)).rgb;
    vec3 m = texture(uSrc, vUv + t * vec2(1.0, 1.0)).rgb;
    vec3 r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    if (uThreshold >= 0.0) {
        float lum = luminance(r);
        float knee = uThreshold * 0.5 + 1e-4;
        float soft = clamp(lum - uThreshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee);
        float contrib = max(soft, lum - uThreshold) / max(lum, 1e-5);
        r *= contrib;
    }
    if (any(isnan(r))) r = vec3(0.0);
    fragColor = vec4(r, 1.0);
}
