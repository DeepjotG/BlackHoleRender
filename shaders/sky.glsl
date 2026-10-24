#ifndef SKY_GLSL
#define SKY_GLSL
#include "common.glsl"
#include "blackbody.glsl"
// Deep-space backdrop: baked cubemap (galaxies, nebulae, Milky Way) + analytic stars.

uniform samplerCube uSkyCube;
uniform float uSkyBrightness;
uniform float uStarDensity;
uniform float uStarBrightness;

// Screen->sky Jacobian of the escaped direction, used to filter the sky and to
// render stars with the correct lensing magnification.
struct Footprint {
    vec3 a, b;               // dFdx / dFdy of the escaped direction
    float Gaa, Gab, Gbb;     // Gram matrix
    float det;               // = |a x b|^2  (footprint solid angle squared)
    float mu;                // magnification = unlensed pixel solid angle / footprint
};

Footprint makeFootprint(vec3 a, vec3 b, float omega0) {
    Footprint f;
    f.a = a; f.b = b;
    f.Gaa = dot(a, a); f.Gab = dot(a, b); f.Gbb = dot(b, b);
    f.det = max(f.Gaa * f.Gbb - f.Gab * f.Gab, 1e-30);
    f.mu = omega0 / sqrt(f.det);
    return f;
}

// One layer of procedurally placed stars. Stars live in cells of a 3D lattice that
// intersect the unit sphere (no cube-face seams). Each star is a Gaussian in sky space
// convolved with a ~0.67 px screen-space Gaussian; the pixel value is F * mu * G, which
// conserves surface brightness so stars brighten and stretch near the Einstein ring.
vec3 starLayer(vec3 d, Footprint fp, float N, float prob, float flux, float sigStar, float omega0, uint salt, float gShift) {
    float footprintCells = pow(fp.det, 0.25) * N;
    float w = smoothstep(0.8, 2.5, footprintCells);           // large footprint -> use the mean sky
    vec3 avg = blackbody(5600.0 * gShift).rgb * (prob * N * N * 4.65 * flux * omega0);
    vec3 sum = vec3(0.0);
    if (w < 1.0) {
        float invDet = 1.0 / fp.det;
        float iGaa = fp.Gbb * invDet, iGab = -fp.Gab * invDet, iGbb = fp.Gaa * invDet;
        float s2 = sigStar * sigStar;
        const float px2 = 0.45;                                 // sigma_px^2
        float Saa = px2 + s2 * iGaa, Sab = s2 * iGab, Sbb = px2 + s2 * iGbb;
        float detS = max(Saa * Sbb - Sab * Sab, 1e-12);
        float invDetS = 1.0 / detS;
        float iSaa = Sbb * invDetS, iSab = -Sab * invDetS, iSbb = Saa * invDetS;
        float norm = fp.mu / (TWO_PI * sqrt(detS));
        vec3 p = d * N;
        ivec3 c0 = ivec3(floor(p));
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            ivec3 c = c0 + ivec3(dx, dy, dz);
            uvec3 h = pcg3d(uvec3(c) + uvec3(salt, salt * 7u + 1u, salt * 13u + 2u));
            vec3 u = vec3(h) * (1.0 / 4294967296.0);
            if (u.x > prob) continue;
            vec3 u2 = vec3(pcg3d(h)) * (1.0 / 4294967296.0);
            vec3 q = (vec3(c) + u2) / N;
            float ql = length(q);
            if (abs(ql - 1.0) > 0.5 / N) continue;              // keep a thin shell -> uniform density
            vec3 delta = q / ql - d;
            float ra = dot(fp.a, delta), rb = dot(fp.b, delta);
            float sp = iGaa * ra + iGab * rb;                   // screen offset in pixels
            float sq = iGab * ra + iGbb * rb;
            float e = iSaa * sp * sp + 2.0 * iSab * sp * sq + iSbb * sq * sq;
            if (e > 30.0) continue;
            float F = flux * pow(max(u.y, 1e-4), -0.83);        // heavy-tailed brightness
            float T = exp(mix(log(2600.0), log(30000.0), pow(u.z, 2.2)));
            sum += blackbody(T * gShift).rgb * (F * norm * exp(-0.5 * e));
        }
    }
    return mix(sum, avg, w);
}

vec3 sampleSky(vec3 d, vec3 a, vec3 b, float omega0, float rcam) {
    // Static observers near the hole see the sky blueshifted and brightened.
    float g = mix(1.0, inversesqrt(max(1.0 - 2.0 / rcam, 1e-4)), uDopplerStrength);
    float g4 = g * g * g * g;
    vec3 col = textureGrad(uSkyCube, d, a, b).rgb * (uSkyBrightness * g4);
    Footprint fp = makeFootprint(a, b, omega0);
    float sb = uStarBrightness * g4;
    col += starLayer(d, fp, 180.0, 0.35 * uStarDensity, 0.012 * sb, 0.0004, omega0, 11u, g);
    col += starLayer(d, fp, 42.0, 0.30 * uStarDensity, 0.25 * sb, 0.0012, omega0, 23u, g);
    return col;
}
#endif
