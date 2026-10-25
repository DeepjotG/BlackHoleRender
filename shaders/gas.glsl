#ifndef GAS_GLSL
#define GAS_GLSL
#include "blackbody.glsl"
// Gas of the star-disruption simulation (src/tde/TdeSim.h rendering contract): up to GAS_MAX cube
// volumes, RGBA16F texels with r = absorption per M, g = temperature / 1e4 K, b = emission
// strength multiplier. Overlapping volumes add.

const int GAS_MAX = 4;
uniform int uGasCount;
uniform sampler3D uGasTex[GAS_MAX];
uniform vec4 uGasBox[GAS_MAX];      // xyz centre (M, renderer frame), w half-size (M)
uniform float uGasRes[GAS_MAX];     // texels per edge
uniform float uGasBrightness;       // artistic emission scale
uniform float uGasOpacity;          // artistic absorption scale
uniform int uGasLive[GAS_MAX];      // 0: the volume holds (almost) nothing and is not worth marching into
layout(std430, binding = 7) readonly buffer GasOccupancy { uint gasOcc[]; };   // see gas_peak.comp
uniform int uGasOccOk;              // 1 when the occupancy blocks are valid this frame
uniform vec3 uGasUnionLo, uGasUnionHi;   // box around every live volume (M, renderer frame)
uniform float uGasRefLen;           // M: one length for every volume (8 star radii), so the same gas looks
                                    // the same in every grid and nothing changes where grids hand over

// Cell size (M) of the finest volume containing p, or 0 when p is outside every volume.
// > 0: cell size (M) of the finest live volume whose block at p holds gas (step finely);
// < 0: p is inside live volumes but only in empty blocks: minus a safe stride (one block, M);
// 0: outside every live volume.
float gasCellSize(vec3 p) {
    float cell = 0.0, empty = 0.0;
    for (int i = 0; i < uGasCount; ++i) {
        if (uGasLive[i] == 0) continue;
        vec3 d = abs(p - uGasBox[i].xyz);
        float hs = uGasBox[i].w;
        if (max(d.x, max(d.y, d.z)) < hs) {
            float c = 2.0 * hs / max(uGasRes[i], 1.0);
            bool full = true;
            if (uGasOccOk != 0) {
                int nb = int(uGasRes[i] + 0.5) / 8;
                ivec3 b = clamp(ivec3((p - uGasBox[i].xyz + hs) / (2.0 * hs) * float(nb)), ivec3(0), ivec3(nb - 1));
                full = gasOcc[i * 512 + (b.z * nb + b.y) * nb + b.x] != 0u;
            }
            if (full) cell = (cell == 0.0) ? c : min(cell, c);
            else empty = (empty == 0.0) ? 4.0 * c : min(empty, 4.0 * c);   // half a block: never jumps a block
        }
    }
    return cell > 0.0 ? cell : -empty;
}

// True when the segment a -> b touches the box [lo, hi].
bool gasSegmentHitsBox(vec3 a, vec3 b, vec3 lo, vec3 hi) {
    vec3 d = b - a;
    vec3 inv = vec3(1.0) / mix(d, vec3(1e-12), lessThan(abs(d), vec3(1e-12)));
    vec3 t0 = (lo - a) * inv, t1 = (hi - a) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
    return tf >= max(tn, 0.0) && tn <= 1.0;
}
// True when the ray from a along unit direction d meets the box [lo, hi].
bool gasRayHitsBox(vec3 a, vec3 d, vec3 lo, vec3 hi) {
    vec3 inv = vec3(1.0) / mix(d, vec3(1e-12), lessThan(abs(d), vec3(1e-12)));
    vec3 t0 = (lo - a) * inv, t1 = (hi - a) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
    return tf >= max(tn, 0.0);
}

// Distance (M) along unit direction rd from p to the nearest entry of a volume that does not contain
// p, or 1e30: rays inside a coarse volume must still land on finer ones ahead instead of stepping over.
float gasEntryDistance(vec3 p, vec3 rd) {
    float best = 1e30;
    for (int i = 0; i < uGasCount; ++i) {
        if (uGasLive[i] == 0) continue;
        vec3 lo = uGasBox[i].xyz - vec3(uGasBox[i].w), hi = uGasBox[i].xyz + vec3(uGasBox[i].w);
        vec3 inv = vec3(1.0) / mix(rd, vec3(1e-12), lessThan(abs(rd), vec3(1e-12)));
        vec3 t0 = (lo - p) * inv, t1 = (hi - p) * inv;
        vec3 tmin = min(t0, t1), tmax = max(t0, t1);
        float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
        if (tn > 0.0 && tf >= tn) best = min(best, tn);
    }
    return best;
}

// Absorbs and emits over a path element of length ds at p (all volumes containing p).
void gasSample(vec3 p, float ds, inout vec3 color, inout float transmittance) {
    // Gather the gas of every live volume at p first (overlaps are already apportioned between
    // volumes), then texture and shade it once: the noise and colour lookups are the expensive part.
    float kSum = 0.0, kb = 0.0, kT = 0.0, rawSum = 0.0;
    int frame = -1;   // finest volume holding gas here: the frame of the turbulence texture
    for (int i = 0; i < uGasCount; ++i) {
        if (uGasLive[i] == 0) continue;   // also here: samples taken inside a live volume overlap the others
        vec3 uvw = (p - uGasBox[i].xyz) / (2.0 * uGasBox[i].w) + 0.5;
        if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) continue;
        vec4 g = texture(uGasTex[i], uvw);
        // Fade out over the outer 1.5 texels so a volume's box never shows as a hard edge.
        vec3 edge = min(uvw, 1.0 - uvw) * uGasRes[i];
        float fade = smoothstep(0.5, 2.0, min(edge.x, min(edge.y, edge.z)));
        float raw = max(g.r, 0.0) * fade;
        if (raw <= 0.0) continue;
        if (frame < 0) frame = i;
        rawSum += raw;
        kb += raw * max(g.b, 0.0);
        kT += raw * max(g.g, 0.0);
    }
    if (frame < 0) return;
    float dn = clamp(rawSum * uGasRefLen / 0.5, 0.0, 1.0);          // density on a 0..1 scale
    if (dn < 0.01) return;   // too thin to see or to dim anything: skip the texture lookups
    kSum = rawSum * uGasOpacity;
    float bMean = kb / rawSum, tMean = kT / rawSum;
    // Turbulent texture: the grids resolve the debris' shape but not its fine structure. Billows
    // (a noise cell per volume half-size) mottle the emission and temperature, and
    // texel-scale detail noise erodes the thin outskirts of the gas into wisps (as for game clouds):
    // dense cores stay solid, tenuous regions break up instead of reading as a soft blur.
    vec3 q = (p - uGasBox[frame].xyz) / uGasBox[frame].w;
    float turb = smoothstep(0.25, 0.75, texture(uNoise3d, q * 0.8 + vec3(0.0, uTime * 0.01, 0.0)).r);   // billows
    float nd = texture(uNoise3d, q * (uGasRes[frame] / 11.0) + vec3(0.71, 0.29, 0.53) + vec3(uTime * 0.03)).r;
    float er = (1.0 - smoothstep(0.3, 0.7, nd)) * 0.7 * (1.0 - turb * 0.5);
    float dEroded = clamp((dn - er) / max(1.0 - er, 0.05), 0.0, 1.0);
    float k = kSum * (dEroded / dn);
    float a = 1.0 - exp(-k * ds);
    // Emission grows with density squared until the gas is optically thick over the reference length
    // (as for free-free emission): dense streams and the forming disk glow, while tenuous envelopes
    // stay faint smoke instead of a glowing haze.
    float glow = bMean * dEroded * (0.25 + 1.5 * turb);
    vec4 bb = blackbody(tMean * 1e4 * (0.85 + 0.3 * turb));
    // Contract: color += T * (1 - exp(-r ds)) * B(T_gas) * b; T *= exp(-r ds). The brightest debris
    // (collision flares, ~100x the stream) is compressed, E / (1 + E / 8), so its structure stays
    // visible instead of burning out to white; dim stream gas is unchanged.
    vec3 e = bb.rgb * (bb.a * glow * uGasBrightness);
    e /= 1.0 + dot(e, vec3(0.2126, 0.7152, 0.0722)) / 8.0;
    color += transmittance * a * e;
    transmittance *= 1.0 - a;
}
#endif
