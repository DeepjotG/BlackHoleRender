#ifndef TDE_PARAMS_GLSL
#define TDE_PARAMS_GLSL
// The star-disruption simulation's per-step parameters (src/tde/TdeSim.cpp, ParamsUbo), shared by its
// compute shaders (tde_common.glsl) and the particle renderer (gas_splat.comp).
const int NGRID = 4;
layout(std140, binding = 3) uniform TdeParams {
    vec4 uGridDep[NGRID];    // deposit geometry: xyz centre, w half size (<= 0: grid inactive)
    vec4 uGridFld[NGRID];    // geometry the field buffers were built with
    vec4 uRamp;              // per grid: width of the ownership ramp inside its boundary (M)
    vec4 uEvent;             // q, R, r_p, r_t
    vec4 uStep;              // dt (coordinate time), eta, max substeps, 1 on the last step of a frame
    vec4 uMono;              // d_max, softening, enabled, core radius d_c (centroid of the core)
    vec4 uScale;             // fixed-point mass per particle, energy scale, heat scale, particle count
    vec4 uPhys;              // black hole gravity (0/1), viscosity C, cooling k (orbits), escape radius
    vec4 uLimit;             // acceleration cap factor, C_cfl, C_acc, C_orb
    vec4 uVis;               // rhoStar (3/(4 pi R^3)), Teff, uStar (q/R), dissipation unit
    vec4 uPoly;              // Kp (P/rho = Kp rho'^(1/n)), 1/n, r_p (disk temperature), unused
    vec4 uExtra;             // pressure scale (diagnostics, 1 = normal), noise damping kappa, self-gravity weight
                             // (0..1, see selfGravity()), unused
    vec4 uFrozen;            // frozen remnant: xyz centre, w = 1 while it exists
    vec4 uFrozenVel;         // xyz its proper velocity, w = its radius (gas falling inside joins it)
    ivec4 uInfo;             // particle count, restriction ratio (nest), flags, 1 while a self-bound core exists
                             // (its gas then counts with the core's orbital energy in the bound fraction)
};
#endif
