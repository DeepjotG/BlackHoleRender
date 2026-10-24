#version 460 core
// Internal-resolution HDR -> window resolution, with contrast-adaptive sharpening.
in vec2 vUv;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uSrcTexel;
uniform float uSharpen;
void main() {
    vec3 c = texture(uSrc, vUv).rgb;
    if (uSharpen <= 0.001) { fragColor = vec4(c, 1.0); return; }
    vec3 n = texture(uSrc, vUv + vec2(0.0, -uSrcTexel.y)).rgb;
    vec3 s = texture(uSrc, vUv + vec2(0.0, uSrcTexel.y)).rgb;
    vec3 e = texture(uSrc, vUv + vec2(uSrcTexel.x, 0.0)).rgb;
    vec3 w = texture(uSrc, vUv + vec2(-uSrcTexel.x, 0.0)).rgb;
    vec3 mn = min(min(min(n, s), min(e, w)), c);
    vec3 mx = max(max(max(n, s), max(e, w)), c);
    // Adaptive weight: sharpen less where local contrast is already high (avoids ringing on stars).
    vec3 contrast = (mx - mn) / (mx + 0.05);
    float wgt = uSharpen * 0.7 * (1.0 - clamp(dot(contrast, vec3(1.0 / 3.0)), 0.0, 1.0));
    vec3 blur = (n + s + e + w) * 0.25;
    vec3 sharp = c + (c - blur) * wgt * 2.0;
    fragColor = vec4(clamp(sharp, mn, mx), 1.0);
}
