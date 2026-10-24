#ifndef SKY_GLSL
#define SKY_GLSL
#include "common.glsl"
#include "blackbody.glsl"
// Deep-space backdrop: baked cubemap (galaxies, nebulae, Milky Way) + analytic stars.

uniform samplerCube uSkyCube;
uniform float uSkyBrightness;
uniform float uStarDensity;
uniform float uStarBrightness;

// Screen->sky Jacobian of the escaped direction, used to filter the sky and to render
// stars with the correct lensing magnification. Built from a QR factorisation of J = [a b]:
// near the Einstein ring the two derivative vectors are almost parallel and the Gram
// determinant |a|^2|b|^2 - (a.b)^2 cancels catastrophically (NaN/garbage pixels), whereas
// the perpendicular part of b is well conditioned.
struct Footprint {
    vec3 a, b;               // dFdx / dFdy of the escaped direction (for textureGrad)
    vec3 e1, e2;             // orthonormal basis of the footprint in sky space
    float r11, r12, r22;     // inverse R factor: screen offset = Rinv * (e1.delta, e2.delta)
    float Gi11, Gi12, Gi22;  // inverse Gram matrix = Rinv * Rinv^T
    float detGi;             // (r11 * r22)^2, exact
    float det;               // (footprint solid angle)^2 = (|a| |b_perp|)^2
    float mu;                // magnification = unlensed pixel solid angle / footprint
};

Footprint makeFootprint(vec3 d, vec3 a, vec3 b, float omega0) {
    Footprint f;
    f.a = a; f.b = b;
    // Bound the magnification per axis at 1e3 so the inverse Jacobian stays finite in float.
    float minAxis = 1e-3 * sqrt(omega0);
    float laRaw = length(a);
    float la = max(laRaw, minAxis);
    f.e1 = (laRaw > 1e-12) ? a / laRaw : normalize(cross(d, vec3(0.0, 1.0, 0.0) + vec3(1e-3)));
    float b1 = dot(b, f.e1);
    vec3 bp = b - b1 * f.e1;
    float lbpRaw = length(bp);
    float lbp = max(lbpRaw, minAxis);
    f.e2 = (lbpRaw > 1e-3 * minAxis) ? bp / lbpRaw : normalize(cross(d, f.e1));
    f.r11 = 1.0 / la;
    f.r12 = -b1 / (la * lbp);
    f.r22 = 1.0 / lbp;
    f.Gi11 = f.r11 * f.r11 + f.r12 * f.r12;
    f.Gi12 = f.r12 * f.r22;
    f.Gi22 = f.r22 * f.r22;
    f.detGi = (f.r11 * f.r22) * (f.r11 * f.r22);
    float area = la * lbp;
    f.det = area * area;
    f.mu = min(omega0 / area, 1e4);
    return f;
}

// One layer of procedurally placed stars. Stars live in cells of a 3D lattice that
// intersect the unit sphere (no cube-face seams). Each star is a Gaussian in sky space
// convolved with a ~0.67 px screen-space Gaussian; the pixel value is F * mu * G, which
// conserves surface brightness so stars brighten and stretch near the Einstein ring.
vec3 starLayer(vec3 d, Footprint fp, float N, float prob, float flux, float sigStar, float omega0, uint salt, float gShift) {
    // Footprint size in cells (geometric mean of its axes). Near the Einstein ring the footprint
    // is huge radially but tiny tangentially; keying on the mean keeps magnified star arcs in
    // per-star mode (keying on the largest axis chops them into dots).
    float footprintCells = pow(fp.det, 0.25) * N;
    float w = smoothstep(0.8, 2.5, footprintCells);           // large footprint -> use the mean sky
    const float MEAN_FLUX = 9.517;                            // E[1 / max(u, 2e-4)] = 1 + ln(5000)
    vec3 avg = blackbody(5600.0 * gShift).rgb * (prob * N * N * MEAN_FLUX * flux * omega0);
    vec3 sum = vec3(0.0);
    if (w < 1.0) {
        float s2 = sigStar * sigStar;
        const float px2 = 0.45;                                 // sigma_px^2
        // Sigma = px2 * I + s2 * Ginv ; its determinant in a cancellation-free form.
        float Saa = px2 + s2 * fp.Gi11, Sab = s2 * fp.Gi12, Sbb = px2 + s2 * fp.Gi22;
        float detS = px2 * px2 + px2 * s2 * (fp.Gi11 + fp.Gi22) + s2 * s2 * fp.detGi;
        float invDetS = 1.0 / detS;
        float iSaa = Sbb * invDetS, iSab = -Sab * invDetS, iSbb = Saa * invDetS;
        float norm = fp.mu / (TWO_PI * sqrt(detS));
        vec3 p = d * N;
        ivec3 c0 = ivec3(floor(p));
        float sIn2 = (1.0 - 0.5 / N) * (1.0 - 0.5 / N), sOut2 = (1.0 + 0.5 / N) * (1.0 + 0.5 / N);
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            ivec3 c = c0 + ivec3(dx, dy, dz);
            uvec3 h = pcg3d(uvec3(c) + uvec3(salt, salt * 7u + 1u, salt * 13u + 2u));
            // One hash per cell: six 16-bit uniforms from its 96 bits.
            float ux = float(h.x >> 16u) * (1.0 / 65536.0);
            if (ux > prob) continue;
            vec3 u2 = vec3(uvec3(h.x & 0xffffu, h.y >> 16u, h.y & 0xffffu)) * (1.0 / 65536.0);
            vec3 u = vec3(ux, float(h.z >> 16u) * (1.0 / 65536.0), float(h.z & 0xffffu) * (1.0 / 65536.0));
            vec3 q = (vec3(c) + u2) / N;
            float ql2 = dot(q, q);
            if (ql2 < sIn2 || ql2 > sOut2) continue;            // keep a thin shell -> uniform density
            vec3 delta = q * inversesqrt(ql2) - d;
            float d1 = dot(fp.e1, delta), d2 = dot(fp.e2, delta);
            float sp = fp.r11 * d1 + fp.r12 * d2;               // screen offset in pixels
            float sq = fp.r22 * d2;
            float e = iSaa * sp * sp + 2.0 * iSab * sp * sq + iSbb * sq * sq;
            if (!(e < 30.0)) continue;                          // also skips NaN/inf
            float F = flux * pow(max(u.y, 2e-4), -1.0);         // heavy-tailed brightness
            float T = exp(mix(log(3000.0), log(22000.0), pow(u.z, 1.7)));
            vec3 sc = mix(vec3(1.0), blackbody(T * gShift).rgb, 0.7);
            sum += sc * (F * norm * exp(-0.5 * e));
        }
    }
    if (any(isnan(sum)) || any(isinf(sum))) sum = vec3(0.0);
    sum = min(sum, vec3(64.0));                               // no single-sample fireflies in the accumulation
    return mix(sum, avg, w);
}

vec3 sampleSky(vec3 d, vec3 a, vec3 b, float omega0, float rcam) {
    // Static observers near the hole see the sky blueshifted and brightened.
    float g = mix(1.0, inversesqrt(max(1.0 - 2.0 / rcam, 1e-4)), uDopplerStrength);
    float g4 = g * g * g * g;
    vec3 col = textureGrad(uSkyCube, d, a, b).rgb * (uSkyBrightness * g4);
    Footprint fp = makeFootprint(d, a, b, omega0);
    float sb = uStarBrightness * g4;
    col += starLayer(d, fp, 200.0, 0.40 * uStarDensity, 0.0045 * sb, 0.0004, omega0, 11u, g);
    col += starLayer(d, fp, 48.0, 0.25 * uStarDensity, 0.06 * sb, 0.0011, omega0, 23u, g);
    return col;
}
#endif
