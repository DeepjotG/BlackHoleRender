#version 460 core
// Order-independent compositing of the gas splats (weighted blended transparency): target 0 adds
// colour x opacity and opacity, target 1 multiplies the transmittance (blend ZERO, ONE_MINUS_SRC_COLOR).
in vec2 vOff;
flat in vec4 vInv;   // inverse covariance (xx, xy, yy), peak optical depth
flat in vec3 vCol;
layout(location = 0) out vec4 oAccum;
layout(location = 1) out vec4 oReveal;
void main() {
    float q = vInv.x * vOff.x * vOff.x + 2.0 * vInv.y * vOff.x * vOff.y + vInv.z * vOff.y * vOff.y;
    if (q > 6.25) discard;
    float a = 1.0 - exp(-vInv.w * exp(-q));
    oAccum = vec4(vCol * a, a);
    oReveal = vec4(a);
}
