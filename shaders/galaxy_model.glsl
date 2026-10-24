#ifndef GALAXY_MODEL_GLSL
#define GALAXY_MODEL_GLSL
// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
//
// GPU twin of src/universe/GalaxyModel.cpp (detail::GalaxyEval::sampleLocal). The constants come
// from GalaxyEval::pack; every formula here must match the C++ one (tests/galaxy_model_tests.cpp and
// the BHS_GALAXY_SELFTEST check in UniverseScene compare them).
//
// Packing (kpc, 1/kpc; light prefactors in L_sun/pc^3, already multiplied by the colour):
//   0 centre rel. viewpoint, type       1 normal, bounding radius      2 major, 1/h
//   3 old light, 1/(2 hz)               4 young light, 1/h_young        5 HII light, 1/(2 hz_young)
//   6 bulge light, a_bulge              7 bar light, 1/q_bulge
//   8 arm count, 1/tan(pitch), ln(R_ref), phase      9 arm k, arm C-1, arm norm, old-disk C-1
//  10 old norm, dust C-1, dust norm, HII arm norm   11 cutoff R, taper R, bar-lane tau, dust tau0
//  12 1/lambda young, amp young, amp old, 1/lambda dust
//  13 amp dust, 1/lambda HII, 1/(2 hz_dust), noise seed
//  14 cos bar, sin bar, 1/a_bar (0 = no bar), 1/b_bar   15 1/c_bar, 1/(2 hz_HII), arm-noise K (0 = 3D noise), cells around
#include "common.glsl"

struct GalaxyGpu { vec4 data[16]; };

vec3 galaxyCenterKpc(in GalaxyGpu g) { return g.data[0].xyz; }
float galaxyBoundRadiusKpc(in GalaxyGpu g) { return g.data[1].w; }
vec3 galaxyNormal(in GalaxyGpu g) { return g.data[1].xyz; }
// Thickness scale of the dust layer (kpc); large for ellipticals, which have no disk.
float galaxyThinKpc(in GalaxyGpu g) { return abs(g.data[0].w - 3.0) < 0.5 ? 1e6 : 0.5 / g.data[13].z; }

float gmLattice(ivec3 c, uint salt) {
    uint h = uint(c.x) * 0x8da6b343u + uint(c.y) * 0xd8163841u + uint(c.z) * 0xcb1ab31fu + salt * 0x165667b1u;
    return float(pcg(h)) * (2.0 / 4294967296.0) - 1.0;
}
// Value noise; the lattice's y index is wrapped by `mask` (period mask + 1; -1 = no wrap).
float gmNoiseM(vec3 p, uint salt, int mask) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec3 c = ivec3(i);
    int y0 = c.y & mask, y1 = (c.y + 1) & mask;
    float a = gmLattice(ivec3(c.x, y0, c.z), salt), b = gmLattice(ivec3(c.x + 1, y0, c.z), salt);
    float cc = gmLattice(ivec3(c.x, y1, c.z), salt), d = gmLattice(ivec3(c.x + 1, y1, c.z), salt);
    float e = gmLattice(ivec3(c.x, y0, c.z + 1), salt), ff = gmLattice(ivec3(c.x + 1, y0, c.z + 1), salt);
    float gg = gmLattice(ivec3(c.x, y1, c.z + 1), salt), hh = gmLattice(ivec3(c.x + 1, y1, c.z + 1), salt);
    return mix(mix(mix(a, b, u.x), mix(cc, d, u.x), u.y), mix(mix(e, ff, u.x), mix(gg, hh, u.x), u.y), u.z);
}
float gmNoise(vec3 p, uint salt) { return gmNoiseM(p, salt, -1); }
float gmLmgf(float s) { return s * s * (0.0815444 + s * (-0.0020730 - 0.00011765 * s)); }
float gmSech2(float u) { float e = exp(-2.0 * abs(u)); float d = 1.0 + e; return 4.0 * e / (d * d); }
float gmSmooth(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }

// pKpc: sample position relative to the viewpoint (kpc, galactic-frame axes). lodKpc > 0 replaces
// structure smaller than lodKpc by its mean. Outputs: starlight and HII emission (L_sun / pc^3,
// linear RGB) and V-band optical depth per pc.
void galaxySampleLod(in GalaxyGpu g, vec3 pKpc, float lod, out vec3 starLight, out vec3 nebula, out float tau) {
    vec3 dd = pKpc - g.data[0].xyz;
    vec3 n = g.data[1].xyz, mj = g.data[2].xyz;
    float x = dot(dd, mj), y = dot(dd, n), z = dot(dd, cross(n, mj));
    float R0 = sqrt(x * x + z * z);
    float R = max(R0, 1e-5);
    starLight = vec3(0.0);
    nebula = vec3(0.0);
    tau = 0.0;

    // Bulge (the whole of an elliptical).
    float aB = g.data[6].w;
    float yq = y * g.data[7].w;
    float mm = max(sqrt(R * R + yq * yq), 0.02 * aB);
    float ma = mm + aB;
    float bul = aB / (mm * ma * ma * ma);
    starLight = g.data[6].rgb * bul;
    if (abs(g.data[0].w - 3.0) < 0.5) return;

    float theta = R0 > 1e-6 ? atan(z, x) : 0.0;
    float psi = g.data[8].x * (theta - (log(R) - g.data[8].z) * g.data[8].y - g.data[8].w);
    float armK = g.data[9].x;
    float A = pow(max(0.5 + 0.5 * cos(psi), 0.0), armK);
    float cutR = g.data[11].x;
    float cut = 1.0 - gmSmooth(0.9 * cutR, 1.1 * cutR, R);
    vec3 off = vec3(float(pcg(uint(g.data[13].w) * 3u + 1u)), float(pcg(uint(g.data[13].w) * 3u + 2u)),
                    float(pcg(uint(g.data[13].w) * 3u + 3u))) * (1000.0 / 4294967296.0);
    vec3 xyz = vec3(x, y, z);

    // Clump noise: arm-aligned (unwound log-polar) coordinates for spirals, plain 3D otherwise.
    float invLamY = g.data[12].x;
    float armK1 = g.data[15].z;
    vec3 Q1;
    float lam1;
    int mask1 = -1, mask2 = -1;
    if (armK1 > 0.0) {
        float Rc = max(R, 0.3 / g.data[2].w);
        float thU = theta - (log(R) - g.data[8].z) * g.data[8].y;
        float P1 = g.data[15].w;
        Q1 = vec3(log(Rc) * armK1, thU * (P1 / 6.28318530717959), y * invLamY * 0.7) + off;
        lam1 = (6.28318530717959 / (P1 * g.data[8].y * sqrt(1.0 + 1.0 / (g.data[8].y * g.data[8].y)))) * Rc;
        mask1 = int(P1) - 1;
        mask2 = 2 * int(P1) - 1;
    } else {
        Q1 = xyz * invLamY + off;
        lam1 = 1.0 / invLamY;
    }
    vec3 Q2 = Q1 * 2.0 + vec3(7.1, 3.3, 5.7);
    float wy1 = clamp(2.0 - 2.0 * lod / lam1, 0.0, 1.0);
    float wy2 = clamp(2.0 - 4.0 * lod / lam1, 0.0, 1.0);
    float n1 = wy1 > 0.0 ? gmNoiseM(Q1, 11u, mask1) : 0.0;
    float n2 = wy2 > 0.0 ? gmNoiseM(Q2, 12u, mask2) : 0.0;
    float sY1 = g.data[12].y * 0.65 * wy1, sY2 = g.data[12].y * 0.35 * wy2, sO = g.data[12].z * wy1;
    float clumpY = exp(sY1 * n1 + sY2 * n2 - gmLmgf(sY1) - gmLmgf(sY2));
    float clumpO = exp(sO * n1 - gmLmgf(sO));

    float old = exp(-R * g.data[2].w) * cut * gmSech2(y * g.data[3].w) * (1.0 + g.data[9].w * A) * g.data[10].x * clumpO;
    float taper1 = g.data[11].y;
    float taper = taper1 > 0.0 ? gmSmooth(0.35 * taper1, taper1, R) : 1.0;
    float radY = exp(-R * g.data[4].w) * cut * taper;
    float Ay = A * A;   // young stars: narrower arms
    float young = radY * gmSech2(y * g.data[5].w) * (1.0 + g.data[9].y * Ay) * g.data[9].z * clumpY;

    float bar = 0.0, xb = 0.0, zb = 0.0;
    bool barred = g.data[14].z > 0.0;
    if (barred) {
        xb = x * g.data[14].x + z * g.data[14].y;
        zb = -x * g.data[14].y + z * g.data[14].x;
        float u = xb * g.data[14].z, v = zb * g.data[14].w, t = y * g.data[15].x;
        bar = exp(-(u * u) * (u * u) - v * v - t * t);
    }
    starLight += g.data[3].rgb * old + g.data[4].rgb * young + g.data[7].rgb * bar;

    float invLamH = g.data[13].y;
    float wh = clamp(2.0 - 2.0 * lod * invLamH, 0.0, 1.0);
    const float hiiMean = 0.137176;
    float knots = hiiMean;
    if (wh > 0.0) {
        float sm = gmSmooth(0.25, 0.6, gmNoise(xyz * invLamH + off * 1.7, 13u));
        knots = wh * sm * sm + (1.0 - wh) * hiiMean;
    }
    float hiiNorm = g.data[10].w;
    float onArm = hiiNorm > 0.0 ? Ay * Ay * hiiNorm : 1.0;
    nebula = g.data[5].rgb * (radY * gmSech2(y * g.data[15].y) * (knots / hiiMean) * onArm * clumpY);

    float dustT0 = g.data[11].w;
    if (dustT0 > 0.0) {
        float invLamD = g.data[12].w, ampD = g.data[13].x;
        float wd3 = clamp(2.0 - 2.0 * lod * invLamD, 0.0, 1.0);
        float wd4 = clamp(2.0 - 4.0 * lod * invLamD, 0.0, 1.0);
        float wd5 = clamp(2.0 - 8.0 * lod * invLamD, 0.0, 1.0);
        float s1 = ampD * 0.5 * wy1, s2 = ampD * 0.3 * wy2, s3 = ampD * 0.2 * wd3, s4 = ampD * 0.15 * wd4, s5 = ampD * 0.1 * wd5;
        vec3 Pd = xyz * invLamD + off * 0.37;
        float sum = 0.0;
        if (wy1 > 0.0) sum += s1 * gmNoiseM(Q1 + vec3(3.1, 7.7, 1.3), 21u, mask1);
        if (wy2 > 0.0) sum += s2 * gmNoiseM(Q2 + vec3(1.7, 9.2, 4.4), 22u, mask2);
        if (wd3 > 0.0) sum += s3 * gmNoise(Pd, 23u);
        if (wd4 > 0.0) sum += s4 * gmNoise(Pd * 2.0 + vec3(5.3, 1.9, 7.2), 24u);
        if (wd5 > 0.0) sum += s5 * gmNoise(Pd * 4.0 + vec3(2.6, 8.4, 3.7), 25u);
        float clumpD = exp(sum - gmLmgf(s1) - gmLmgf(s2) - gmLmgf(s3) - gmLmgf(s4) - gmLmgf(s5));
        float Ad = pow(max(0.5 + 0.5 * cos(psi - 0.55), 0.0), 1.6 * armK);
        float barLen = barred ? 1.25 / g.data[14].z : 0.0;
        float inner = barred ? 0.25 + 0.75 * gmSmooth(0.5 * barLen, barLen, R) : 1.0;
        float vert = gmSech2(y * g.data[13].z);
        tau = dustT0 * exp(1.0 - R * g.data[2].w) * cut * inner * vert * (1.0 + g.data[10].y * Ad) * g.data[10].z * clumpD;
        if (barred) {
            float bb = 1.0 / g.data[14].w;
            float o2 = (zb + (xb >= 0.0 ? 0.5 : -0.5) * bb) / (0.22 * bb);
            float ax = abs(xb);
            tau += g.data[11].z * exp(-o2 * o2) * gmSmooth(0.15 * barLen, 0.35 * barLen, ax) *
                   (1.0 - gmSmooth(0.85 * barLen, 1.05 * barLen, ax)) * vert * clumpD;
        }
    }
}

// Contract used by the rest of the renderer: full detail, emission = starlight + nebulae.
void galaxySample(in GalaxyGpu g, vec3 pKpc, out vec3 emissivity, out float extinction) {
    vec3 light, neb;
    galaxySampleLod(g, pKpc, 0.0, light, neb, extinction);
    emissivity = light + neb;
}
#endif
