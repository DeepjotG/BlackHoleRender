#version 460 core
// 9-tap tent upsample, additively blended into the next larger level.
in vec2 vUv;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uSrcTexel;
uniform float uScale;
void main() {
    vec2 t = uSrcTexel * 1.0;
    vec3 s = texture(uSrc, vUv + t * vec2(-1.0, -1.0)).rgb;
    s += texture(uSrc, vUv + t * vec2(0.0, -1.0)).rgb * 2.0;
    s += texture(uSrc, vUv + t * vec2(1.0, -1.0)).rgb;
    s += texture(uSrc, vUv + t * vec2(-1.0, 0.0)).rgb * 2.0;
    s += texture(uSrc, vUv).rgb * 4.0;
    s += texture(uSrc, vUv + t * vec2(1.0, 0.0)).rgb * 2.0;
    s += texture(uSrc, vUv + t * vec2(-1.0, 1.0)).rgb;
    s += texture(uSrc, vUv + t * vec2(0.0, 1.0)).rgb * 2.0;
    s += texture(uSrc, vUv + t * vec2(1.0, 1.0)).rgb;
    fragColor = vec4(s * (uScale / 16.0), 1.0);
}
