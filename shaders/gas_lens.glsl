#ifndef GAS_LENS_GLSL
#define GAS_LENS_GLSL
// Lens table of the star-disruption particles (gas_lens.comp builds it, gas_splat.comp reads it).
//
// A fan of light rays leaves the camera in one plane through the black hole, at local angles alpha from
// the direction of the hole (0) to straight away from it (pi), and bends exactly as the tracer's rays do
// (geodesic.glsl). Texel (ray, j) holds log(r) of that ray when it has swept the angle
// phi_j = j * 2 pi / (LENS_PHIS - 1) around the hole: log(2) once it has fallen in, log(LENS_RMAX) once
// it has escaped. For a fixed phi, r grows with alpha, so the image of a point (r, phi) is found by a
// binary search over the fan; phi > pi gives the secondary image (the ray going round the other side).
#include "common.glsl"

const int LENS_RAYS = 1024;
const int LENS_PHIS = 1024;
const float LENS_RMAX = 2.0e5;   // M; points farther out are drawn without lensing
const float LENS_P = 3.0;         // rays crowd round the shadow's edge (higher-order images live there)

// Fan coordinate u in [0, 1] <-> local angle alpha; ac = the shadow's angular radius at the camera.
float lensAlpha(float u, float ac) {
    float uc = ac / PI;
    if (u < uc) return ac - ac * pow((uc - u) / uc, LENS_P);
    return ac + (PI - ac) * pow((u - uc) / (1.0 - uc), LENS_P);
}
float lensDAlphaDu(float u, float ac) {
    float uc = ac / PI;
    if (u < uc) return ac * LENS_P * pow((uc - u) / uc, LENS_P - 1.0) / uc;
    return (PI - ac) * LENS_P * pow((u - uc) / (1.0 - uc), LENS_P - 1.0) / (1.0 - uc);
}
#endif
