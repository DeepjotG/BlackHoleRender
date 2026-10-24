#ifndef TDE_VISUAL_GLSL
#define TDE_VISUAL_GLSL
// The one place that maps the simulated gas onto the rendering contract of src/tde/TdeSim.h:
//   r = absorption coefficient (optical depth per M), g = temperature / 1e4 K, b = emission multiplier.
// Event inputs (uniform block in tde_common.glsl): uVis.x = rhoStar = 3 / (4 pi R^3) (the star's mean
// density in star-mass fractions per M^3), uVis.y = Teff of the star, uPoly.z = pericentre r_p.

// ---- absorption ---------------------------------------------------------------------------------
// alpha = OPAC_C * rho'^(1/3) with rho' in fractions of the star's mass per M^3: no length scale is
// involved, so a blob holding a fraction f of the star has optical depth ~ OPAC_C f^(1/3) whatever
// its size. The star, the stream and the disk therefore look alike for every black hole mass and
// star, and smearing a thin stream over coarse cells changes its optical depth only as
// (cell / width)^(1/3).
const float OPAC_C = 6.0;
// Gas denser than PHOTO_RHO1 (ramping in from PHOTO_RHO0) times the star's mean density is boosted
// so the freshly released star and surviving cores are opaque out to their photosphere, like the
// sphere the tracer draws before release.
const float PHOTO_BOOST = 40.0;
const float PHOTO_RHO0 = 2e-4;
const float PHOTO_RHO1 = 3e-3;
// Cells holding less than NOISE_PART particles' worth of gas fade linearly (hides particle noise).
const float NOISE_PART = 3.0;
const float ALPHA_MAX = 6.0e4;            // RGBA16F range

// ---- temperature (K) and emission multiplier ----------------------------------------------------
const float T_STREAM = 3800.0;            // cold debris expanding away from pericentre: warm orange
const float B_STREAM = 0.45;              // ... and relatively dim
const float T_DISK_PERI = 3.0e4;          // gas on circular orbits at 2 r_p ...
const float T_DISK_SLOPE = 0.75;          // ... hotter inwards as r^-3/4 ...
const float T_DISK_MIN = 1.0e4;           // ... clamped to 1e4 .. 5e4 K
const float T_DISK_MAX = 5.0e4;
const float B_DISK = 1.6;
const float CIRC_VR = 0.08;               // circular flow: (v_r / v_c)^2 well below this ...
const float CIRC_VT = 0.04;               // ... and (v_t / v_c - 1)^2 well below this
const float THETA0 = 0.02;                // thermal over gravitational energy u r: shock-heated gas
const float THETA1 = 0.25;                // blends from stream to disk temperature in this range
const float SHOCK_GAIN = 4.0;             // emission boost per unit heating rate h r^2.5 (heating in
                                          // units of the local virial energy per dynamical time)
const float SHOCK_TEMP_EXP = 0.25;        // temperature rises as (1 + heating)^0.25
const float B_MAX = 24.0;
const float T_MIN = 1200.0;
const float T_MAX = 6.0e4;

// rho: rho' (star-mass fraction per M^3); rhoU, rhoV, rhoH: densities of internal energy, momentum and
// heating rate; p: position (M); nPart: particles' worth of gas in the cell.
vec4 tdeVisual(float rho, float rhoU, vec3 rhoV, float rhoH, vec3 p, float nPart) {
    if (!(rho > 0.0) || nPart <= 1e-3) return vec4(0.0);
    float u = max(rhoU / rho, 0.0);
    vec3 vel = rhoV / rho;
    float h = max(rhoH / rho, 0.0);
    float r = max(length(p), 2.0);

    float dense = smoothstep(PHOTO_RHO0, PHOTO_RHO1, rho / uVis.x);
    float alpha = OPAC_C * pow(rho, 1.0 / 3.0) * (1.0 + PHOTO_BOOST * dense) * min(1.0, nPart / NOISE_PART);

    vec3 rh = p / r;
    float vr = dot(vel, rh);
    float vt = length(vel - vr * rh);
    float vc = inversesqrt(max(r - 3.0, 0.5));
    float dt = vt / vc - 1.0;
    float circ = exp(-(vr * vr) / (CIRC_VR * vc * vc)) * exp(-(dt * dt) / CIRC_VT);
    float hot = smoothstep(THETA0, THETA1, u * r);
    float diskW = max(circ, hot);
    float tDisk = clamp(T_DISK_PERI * pow(2.0 * uPoly.z / r, T_DISK_SLOPE), T_DISK_MIN, T_DISK_MAX);
    float T = mix(T_STREAM, tDisk, diskW);
    float b = mix(B_STREAM, B_DISK, diskW);
    T = mix(T, uVis.y, dense);
    b = mix(b, 1.0, dense);
    float shock = h * r * r * sqrt(r);
    T *= pow(1.0 + shock, SHOCK_TEMP_EXP);
    b += SHOCK_GAIN * shock;
    return vec4(min(alpha, ALPHA_MAX), clamp(T, T_MIN, T_MAX) * 1e-4, min(b, B_MAX), 0.0);
}
#endif
