#version 460 core
// Running average of jittered frames while the camera is still (progressive supersampling).
in vec2 vUv;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uCurrent;
uniform sampler2D uPrevious;
uniform float uWeight;   // weight of the current frame (1 / (n + 1))
void main() {
    vec3 cur = texture(uCurrent, vUv).rgb;
    vec3 prev = texture(uPrevious, vUv).rgb;
    fragColor = vec4(uWeight >= 1.0 ? cur : mix(prev, cur, uWeight), 1.0);
}
