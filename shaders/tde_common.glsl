#ifndef TDE_COMMON_GLSL
#define TDE_COMMON_GLSL
// Shared definitions of the star-disruption compute shaders (src/tde/TdeSim.cpp). The force law,
// lapse and particle sub-cycling mirror src/tde/TdePhysics.h; keep them in sync.
//
// Units: G = c = M_bh = 1, black hole at the origin, renderer frame. Particle masses are equal; grid
// densities are in "star mass fractions per M^3" (rho' = rho / q), which leaves -grad(P)/rho unchanged.
//
// Grids: NGRID cubes of RES^3 cells. Grid 0 follows the debris (the star or its densest remnant),
// grids 1..3 are a black-hole-centred nest whose half sizes grow by a power of two, so that coarse
// cells contain whole fine cells. Every grid holds all the gas inside its box (apart from its
// outermost cell layer): particles deposit into grid 0 (if inside) and into the finest nest level
// containing them, and restriction sums every nest level into the next coarser one. While grid 0 is
// at least twice finer than the nest (uInfo.z = 1) its particles skip the nest and grid 0 is restricted
// into the nest cell by cell instead (tde_g0nest.comp).
//
// Accumulators are integers (no float atomics): per cell one 32-bit mass word and four signed 64-bit
// fixed-point sums (momentum x, y, z, internal energy, heating rate), each stored as two 32-bit words
// (low, high). Per-deposit values stay below 2^31 for typical gas, so the high word needs a second
// atomic only on carries; hot gas uses the full 64-bit range. Nest cells also carry the "owned" part
// of their mass, energy and heating: what grid 0 renders itself, so that the nest renders only the rest.

#ifndef TDE_RES
#define TDE_RES 64
#endif
const int RES = TDE_RES;
const int CELLS = RES * RES * RES;
const int NWORDS = 16;
const int W_MASS = 0;
const int W_MOM = 1;        // x: 1,2  y: 3,4  z: 5,6 (low, high)
const int W_ENER = 7;       // 7,8
const int W_HEAT = 9;       // 9,10
const int W_OWN_MASS = 11;  // owned by grid 0 (nest levels only)
const int W_OWN_ENER = 12;  // 12,13
const int W_OWN_HEAT = 14;  // 14,15

const float GAMMA = 5.0 / 3.0;
const float R_ACCRETE = 2.05;
const float MAX_SPEED = 2.0;
const float MIN_LAPSE = 0.02;
const float TDE_TWO_PI = 6.28318530717959;
const float MOM_SCALE = 536870912.0;   // 2^29 per particle and unit velocity (|v| <= 2)

const int NBINS = 256;       // monopole radial bins (quadratic spacing up to uMono.x)
const int NDT = 128;         // time-step histogram: bin b covers dt in [2^((b - 64) / 2), 2^((b - 63) / 2))
const int TRAJ = 64;         // samples of the core trajectory over one hydro step

// Stats words (binding 8). Cumulative counters survive frames; frame counters are cleared per frame.
const int S_ACCRETED = 0;    // cumulative particles accreted
const int S_ESCAPED = 1;     // cumulative particles removed as escaped
const int S_NAN = 2;         // cumulative particles removed because their state became non-finite
const int S_ACTIVE = 16;     // frame: active particles (last step)
const int S_BOUND = 17;      // frame: active particles with E < 1 (last step)
const int S_DISS = 18;       // frame: dissipated energy, 64-bit (18, 19), units uVis.w per particle
const int S_CAPPED = 20;     // frame: particles that hit the substep cap
const int S_OCC = 21;        // frame: occupied cells per grid (21..24), last deposit
const int S_G0_MASS = 25;    // frame: grid 0 particles' worth of mass (x256), last deposit
const int S_G0_OUTER = 26;   // frame: same, in the outer 1/8 of the box on each side
const int S_LIVE = 27;       // frame: blocks in the dilated set over all grids (last deposit)
const int S_FROZEN = 28;     // frame: particles frozen / thawed / removed by tde_init.comp modes 7-9
// Work counters of the last hydro step of a frame, for the cost model of TdeSim.cpp (deterministic: they
// depend on the particles' state only).
const int S_SUBSTEPS = 29;   // frame: gravity substeps of all particles (tde_update.comp)
const int S_GWRITES = 30;    // frame: global cell updates of the deposit (direct deposits + flushed table slots)
const int S_DTHIST = 64;     // frame: NDT bins of required hydro step
const int S_WORDS = 64 + NDT;

#include "tde_params.glsl"

// Particle states: 0 active, 1 accreted, 2 escaped, 3 unused, 4 frozen (part of a surviving remnant far
// from the hole moving rigidly: xyz is then the offset from uFrozen, the velocity relative to uFrozenVel).
layout(std430, binding = 0) buffer PartPos { vec4 pPos[]; };   // xyz, w = state
layout(std430, binding = 1) buffer PartVel { vec4 pVel[]; };   // xyz proper velocity dx/dtau, w = internal energy u
layout(std430, binding = 2) buffer PartAux { vec4 pAux[]; };   // x initial entropy A0, y heating rate, z u (copy), w rho'
layout(std430, binding = 3) buffer GridAcc { uint gAcc[]; };   // [grid][word][cell]
layout(std430, binding = 4) buffer GridFA { vec4 gFA[]; };     // [grid][cell]: grad P', rho'
layout(std430, binding = 5) buffer GridFB { vec4 gFB[]; };     // [grid][cell]: rho' v, rho' u
// Monopole histogram and table; mCore: particles within d_c of the predicted core position [0], their
// summed offsets (x / d_c * 2^22) and relative velocities (v * 2^24) as 64-bit pairs [1..12], how many
// of them are gravitationally bound to the core [13], particles within 2 d_c [14] and their second
// moments xx yy zz xy xz yz ((x / 2 d_c)^2 * 2^22, 64-bit pairs) [15..26], particles bound to the core
// at any distance [27] (boundToCore()).
const float CORE_XS = 4194304.0;
const float CORE_VS = 16777216.0;
// mRho / mCnt / mP / mAr: hydrostatic set-up at release (tde_init.comp): per radial bin the summed
// grid density seen by the particles (rho / rhoStar * 2^24, 64-bit pairs), their count, the pressure
// at the bin edges and the summed net radial acceleration of the particles.
layout(std430, binding = 6) buffer Mono {
    uint mHist[NBINS + 2];
    float mCum[NBINS + 2];
    uint mCore[32];
    uint mRho[2 * NBINS];
    uint mCnt[NBINS];
    float mP[NBINS + 1];
    uint mAr[2 * NBINS];      // summed net radial acceleration (units q / R^2 * 2^-20), 64-bit pairs
};
layout(std430, binding = 7) buffer Core {
    vec4 cPos;                // xyz core position, w = 1 if valid
    vec4 cVel;                // xyz core proper velocity, w = particles' worth of mass near the peak
    vec4 cInfo;               // x = peak density rho' of grid 0, y = particles within d_c bound to the core,
                              // z = elongation weight (cAxis.w), w = fraction of the star bound to the core
                              // at any distance (mCore[27], the self-bound mass M_b: self-gravity switch)
    vec4 cEndVel;             // xyz predicted core velocity at the end of the hydro step
    vec4 cAxis;               // xyz major axis of the gas near the core, w = elongation weight (0 round)
    vec4 cTraj[TRAJ + 1];     // core position at t = k dt / TRAJ during the current hydro step
};
layout(std430, binding = 8) buffer Stats { uint sWords[S_WORDS]; };

// Block-sparse bookkeeping: the grids are processed in blocks of 8^3 cells. Bit 0 of a block's flag
// word = written by the current deposit (or restriction), bit 1 = within one block of such a block
// (the stencils of the gradient and of the particles' interpolation never leave this dilated set).
// Accumulators outside the previous dilated set are known to be zero, fields there are stale and
// must not be read.
const int BLK = 8;
const int NBLK = RES / BLK;
const int BLOCKS = NBLK * NBLK * NBLK;
layout(std430, binding = 9) buffer Blocks { uint gBlk[]; };   // [grid][block]
layout(std430, binding = 10) buffer GridFC { float gFC[]; };  // [grid][cell]: rho' div(v)

int blockOfCell(int cell) {
    int x = cell % RES, y = (cell / RES) % RES, z = cell / (RES * RES);
    return ((z / BLK) * NBLK + y / BLK) * NBLK + x / BLK;
}
void markTouched(int grid, int cell) {
    int b = grid * BLOCKS + blockOfCell(cell);
    if ((gBlk[b] & 1u) == 0u) atomicOr(gBlk[b], 1u);
}
bool cellLive(int grid, int cell) { return (gBlk[grid * BLOCKS + blockOfCell(cell)] & 2u) != 0u; }

// Adds the signed 64-bit integer (lo, hi) to the accumulator pair at idx.
void accAddPair(int idx, uint lo, uint hi) {
    if (lo == 0u && hi == 0u) return;
    uint old = atomicAdd(gAcc[idx], lo);
    uint hiAdd = hi + ((old + lo < old) ? 1u : 0u);
    if (hiAdd != 0u) atomicAdd(gAcc[idx + CELLS], hiAdd);
}
// Splits round(value) into a signed 64-bit (lo, hi) pair.
uvec2 toPair(float value) {
    float a = abs(value);
    if (!(a >= 0.5)) return uvec2(0u);
    a = min(a, 4.0e18);
    uint hi = uint(a * 2.3283064365386963e-10);
    uint lo = uint(min(a - float(hi) * 4294967296.0 + 0.5, 4294967040.0));
    if (value < 0.0) {
        lo = ~lo + 1u;
        hi = ~hi + (lo == 0u ? 1u : 0u);
    }
    return uvec2(lo, hi);
}

// ------------------------------------------------------------------------------------------------ grids
float gridCell(vec4 geo) { return 2.0 * geo.w / float(RES); }
// Continuous cell coordinate: cell centres sit at integer + 0.5.
vec3 gridCoord(vec4 geo, vec3 x) { return (x - geo.xyz) * (float(RES) / (2.0 * geo.w)) + 0.5 * float(RES); }
bool insideMargin(vec3 s, float m) {
    return all(greaterThanEqual(s, vec3(m))) && all(lessThan(s, vec3(float(RES) - m)));
}
int cellIndex(ivec3 c) { return (c.z * RES + c.y) * RES + c.x; }
int accIndex(int grid, int word, int cell) { return (grid * NWORDS + word) * CELLS + cell; }
// Rendering ownership ramp of a grid: 0 up to 1.5 cells inside its boundary, rising smoothly to 1 over
// `ramp` (M) further inside.
float insideRampGeo(vec4 geo, float ramp, vec3 p) {
    if (geo.w <= 0.0) return 0.0;
    vec3 d = abs(p - geo.xyz);
    float inside = geo.w - max(d.x, max(d.y, d.z));
    float m0 = 1.5 * gridCell(geo);
    return smoothstep(m0, m0 + ramp, inside);
}

// ------------------------------------------------------------------------------------------------ fixed point
// Adds round(value) to the signed 64-bit integer stored at words idx (low) and idx + CELLS (high).
// |value| is clamped below 2^62; NaN adds nothing.
void accAdd64(int idx, float value) {
    uvec2 p = toPair(value);
    accAddPair(idx, p.x, p.y);
}

float decode64(uint lo, uint hi) {
    bool neg = (hi & 0x80000000u) != 0u;
    if (neg) {
        lo = ~lo + 1u;
        hi = ~hi + (lo == 0u ? 1u : 0u);
    }
    float v = float(hi) * 4294967296.0 + float(lo);
    return neg ? -v : v;
}
float accRead64(int idx) { return decode64(gAcc[idx], gAcc[idx + CELLS]); }

// 64-bit unsigned add of word pairs (used by the restriction).
uvec2 add64(uvec2 a, uvec2 b) {
    uint lo = a.x + b.x;
    return uvec2(lo, a.y + b.y + (lo < a.x ? 1u : 0u));
}

// ------------------------------------------------------------------------------------------------ fields
// Trilinear interpolation of the field buffers of grid g at continuous cell coordinate s (needs a
// margin of at least 0.5 cells). fc = rho' div(v).
void sampleFields(int g, vec3 s, out vec4 fa, out vec4 fb, out float fc) {
    vec3 sm = s - 0.5;
    ivec3 i0 = ivec3(floor(sm));
    vec3 f = sm - vec3(i0);
    fa = vec4(0.0);
    fb = vec4(0.0);
    fc = 0.0;
    for (int c = 0; c < 8; ++c) {
        ivec3 o = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        vec3 wv = mix(1.0 - f, f, vec3(o));
        float w = wv.x * wv.y * wv.z;
        int cell = cellIndex(i0 + o);
        if (!cellLive(g, cell)) continue;   // never happens for gas that was just deposited; safety
        int idx = g * CELLS + cell;
        fa += w * gFA[idx];
        fb += w * gFB[idx];
        fc += w * gFC[idx];
    }
}

// ------------------------------------------------------------------------------------------------ gravity
float angMom2(vec3 x, vec3 v) {
    vec3 l = cross(x, v);
    return dot(l, l);
}
// a = -x/r^3 - 3 L^2 x/r^5: reproduces Schwarzschild orbits of massive particles (see TdePhysics.h).
vec3 bhAccel(vec3 x, float L2) {
    float r2 = dot(x, x);
    float r = sqrt(r2);
    return x * (-(1.0 + 3.0 * L2 / r2) / (r2 * r));
}
float specificEnergy(vec3 x, vec3 v) {
    float r = length(x);
    float vr = dot(x, v) / r;
    float e2 = (1.0 - 2.0 / r) * (1.0 + angMom2(x, v) / (r * r)) + vr * vr;
    return sqrt(max(e2, 1e-12));
}
// dtau/dt.
float lapseOf(vec3 x, vec3 v) {
    if (uPhys.x < 0.5) return 1.0;
    float r = length(x);
    if (r <= 2.0) return MIN_LAPSE;
    return clamp((1.0 - 2.0 / r) / specificEnergy(x, v), MIN_LAPSE, 1.0);
}
float bhTimescale(float r, float L2, float speed) {
    float tAcc = r * sqrt(r) * inversesqrt(1.0 + 3.0 * L2 / (r * r));
    return min(tAcc, 2.0 * r / max(speed, 1e-30));
}
float segMinRadius(vec3 a, vec3 b) {
    vec3 d = b - a;
    float dd = dot(d, d);
    float t = dd > 0.0 ? clamp(-dot(a, d) / dd, 0.0, 1.0) : 0.0;
    return length(a + t * d);
}
vec3 clampSpeed(vec3 v) {
    float s = length(v);
    return s > MAX_SPEED ? v * (MAX_SPEED / s) : v;
}

// Enclosed mass fraction around the core from the cumulative monopole table (bin edge b at
// d_max (b / NBINS)^2, linear in d inside a bin).
float monoMass(float d) {
    float dmax = uMono.x;
    if (d >= dmax) return mCum[NBINS];
    float sb = sqrt(d / dmax) * float(NBINS);
    int b = min(int(sb), NBINS - 1);
    float e0 = float(b) / float(NBINS), e1 = float(b + 1) / float(NBINS);
    float d0 = dmax * e0 * e0, d1 = dmax * e1 * e1;
    return mix(mCum[b], mCum[b + 1], clamp((d - d0) / max(d1 - d0, 1e-30), 0.0, 1.0));
}
// Gas bound to the self-gravitating core: slower relative to the core (dv) than the escape speed from the
// gas within its distance d of the core (monopole table of the previous step). The potential of the gas
// outside d is left out, so the binding is slightly underestimated; tides are ignored.
bool boundToCore(vec3 dx, vec3 dv) {
    float d = length(dx);
    return 0.5 * dot(dv, dv) < uEvent.x * monoMass(d) / max(d, uMono.y);
}

// Softened monopole self-gravity of the star's gas around the core centre xc, scaled by uExtra.z: 1 while
// gas is bound to a core (the star, a surviving remnant, the self-gravitating middle of a young stream),
// fading to 0 once almost none is (TdeSim.cpp: a full disruption's stream would otherwise keep pulling
// on itself through a monopole centred on its densest part). `ts` returns the local dynamical time
// sqrt(d^3 / (q M(<d))) with d >= softening.
vec3 selfGravity(vec3 x, vec3 xc, out float ts) {
    vec3 d = x - xc;
    float d2 = dot(d, d);
    float eps = uMono.y;
    float dl = sqrt(d2);
    float k = uEvent.x * clamp(uExtra.z, 0.0, 1.0);
    float m = k * monoMass(dl);
    float de = max(dl, eps);
    ts = de * sqrt(de / max(k * monoMass(de), 1e-30));   // finite even without gravity
    float s2 = d2 + eps * eps;
    vec3 a = d * (-m / (s2 * sqrt(s2)));
    // Near the middle of a filament the monopole's transverse pull 2 q lambda / d is the exact field of
    // a line (Gauss), but its pull along the filament is far too strong (the mass inside d is spread
    // along the line, not at its middle). Remove that component as the gas gets elongated.
    return a - cAxis.w * dot(a, cAxis.xyz) * cAxis.xyz;
}
// Core position at coordinate time t into the current hydro step.
vec3 coreAt(float t) {
    float s = clamp(t / max(uStep.x, 1e-30), 0.0, 1.0) * float(TRAJ);
    int k = min(int(s), TRAJ - 1);
    return mix(cTraj[k].xyz, cTraj[k + 1].xyz, s - float(k));
}
#endif
