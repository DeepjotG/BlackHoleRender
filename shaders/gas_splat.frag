#version 460 core
// Order-independent compositing of the gas splats (weighted blended transparency): target 0 adds
// colour x opacity and opacity, target 1 multiplies the transmittance (blend ZERO, ONE_MINUS_SRC_COLOR).
//
// A splat is not a flat disc: its density is modulated per pixel by the tileable 3D noise, sampled at
// the pixel's position in the gas (the splat's anchor plus the pixel's offset, in the noise's own
// coordinates, which expand with the ejecta and flow along a jet), so neighbouring splats carry one
// continuous cloud structure across their overlaps instead of each showing its own blob.
in vec2 vOff;
flat in vec4 vInv;      // inverse covariance (xx, xy, yy), peak optical depth
flat in vec4 vCol;      // colour, strength of the noise modulation (0: a plain Gaussian)
flat in vec4 vNoise;    // anchor in noise space, noise units per pixel
uniform sampler3D uNoise3d;
uniform vec3 uRight, uUp;   // the camera's screen axes in world space
layout(location = 0) out vec4 oAccum;
layout(location = 1) out vec4 oReveal;
void main() {
    float q = vInv.x * vOff.x * vOff.x + 2.0 * vInv.y * vOff.x * vOff.y + vInv.z * vOff.y * vOff.y;
    if (q > 4.0) discard;
    vec3 wp = vNoise.xyz + (uRight * vOff.x + uUp * vOff.y) * vNoise.w;
    float n = texture(uNoise3d, wp).r;
    float dens = mix(1.0, 2.0 * smoothstep(0.28, 0.72, n), vCol.w);   // mean about 1: the integrated depth is kept
    float a = 1.0 - exp(-vInv.w * exp(-q) * dens);
    vec3 c = vCol.rgb * mix(1.0, 0.6 + 0.4 * dens, vCol.w);           // the knots a little brighter
    oAccum = vec4(c * a, a);
    oReveal = vec4(a);
}
