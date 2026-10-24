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

// Cell size (M) of the finest volume containing p, or 0 when p is outside every volume.
float gasCellSize(vec3 p) {
    float cell = 0.0;
    for (int i = 0; i < uGasCount; ++i) {
        vec3 d = abs(p - uGasBox[i].xyz);
        float hs = uGasBox[i].w;
        if (max(d.x, max(d.y, d.z)) < hs) {
            float c = 2.0 * hs / max(uGasRes[i], 1.0);
            cell = (cell == 0.0) ? c : min(cell, c);
        }
    }
    return cell;
}

// Distance (M) along unit direction rd from p to the nearest volume entry, or 1e30.
float gasEntryDistance(vec3 p, vec3 rd) {
    float best = 1e30;
    for (int i = 0; i < uGasCount; ++i) {
        vec3 lo = uGasBox[i].xyz - vec3(uGasBox[i].w), hi = uGasBox[i].xyz + vec3(uGasBox[i].w);
        vec3 inv = vec3(1.0) / mix(rd, vec3(1e-12), lessThan(abs(rd), vec3(1e-12)));
        vec3 t0 = (lo - p) * inv, t1 = (hi - p) * inv;
        vec3 tmin = min(t0, t1), tmax = max(t0, t1);
        float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
        if (tf >= max(tn, 0.0)) best = min(best, max(tn, 0.0));
    }
    return best;
}

// Absorbs and emits over a path element of length ds at p (all volumes containing p).
void gasSample(vec3 p, float ds, inout vec3 color, inout float transmittance) {
    for (int i = 0; i < uGasCount; ++i) {
        vec3 uvw = (p - uGasBox[i].xyz) / (2.0 * uGasBox[i].w) + 0.5;
        if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) continue;
        vec4 g = texture(uGasTex[i], uvw);
        float k = max(g.r, 0.0) * uGasOpacity;
        if (k <= 0.0 && g.b <= 0.0) continue;
        float a = 1.0 - exp(-k * ds);
        vec4 bb = blackbody(max(g.g, 0.0) * 1e4);
        // Contract: color += T * (1 - exp(-r ds)) * B(T_gas) * b; T *= exp(-r ds).
        color += transmittance * a * bb.rgb * (bb.a * max(g.b, 0.0) * uGasBrightness);
        transmittance *= 1.0 - a;
    }
}
#endif
