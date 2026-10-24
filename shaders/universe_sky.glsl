#ifndef UNIVERSE_SKY_GLSL
#define UNIVERSE_SKY_GLSL
#include "universe_common.glsl"
#include "sky.glsl"
// Sky of the 3D universe for an escaped ray: diffuse light baked from the viewpoint plus the
// real/procedural stars, each drawn with the same lensing-aware footprint as the backdrop stars.

uniform int uUniverse;          // 1 = use the 3D universe sky
uniform samplerCube uUniCube;   // rgb: diffuse light seen from the camera; a: optical depth
uniform float uUniStarScale;    // radiance scale of a 0 mag star
uniform int uUniT;              // star tiles per cube-face edge

struct UStar {
    vec4 dirFlux;
    vec4 color;
};
layout(std430, binding = 4) readonly buffer UStars { UStar ustars[]; };
layout(std430, binding = 5) readonly buffer UTiles { uint utile[]; };   // exclusive offsets, utile[N] = total

vec3 universeStars(vec3 d, Footprint fp, float omega0) {
    // When a pixel covers a large patch of sky (heavily demagnified regions) individual stars
    // average out; their light is negligible there compared with the diffuse glow.
    float tileAngle = 1.5708 / float(uUniT);
    if (pow(fp.det, 0.25) > 2.0 * tileAngle) return vec3(0.0);
    const float s2 = 3e-4 * 3e-4;            // intrinsic angular size of a star (stretches into arcs when lensed)
    const float px2 = 0.45;                  // screen-space PSF variance, px^2
    float Saa = px2 + s2 * fp.Gi11, Sab = s2 * fp.Gi12, Sbb = px2 + s2 * fp.Gi22;
    float detS = px2 * px2 + px2 * s2 * (fp.Gi11 + fp.Gi22) + s2 * s2 * fp.detGi;
    float invDetS = 1.0 / detS;
    float iSaa = Sbb * invDetS, iSab = -Sab * invDetS, iSbb = Saa * invDetS;
    float norm = fp.mu / (TWO_PI * sqrt(detS));

    int f;
    vec2 uv;
    cubeFaceUV(d, f, uv);
    ivec2 ij = clamp(ivec2((uv * 0.5 + 0.5) * float(uUniT)), ivec2(0), ivec2(uUniT - 1));
    int seen[9];
    int nSeen = 0;
    vec3 sum = vec3(0.0);
    for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di) {
        ivec2 n = ij + ivec2(di, dj);
        int tile;
        if (n.x >= 0 && n.y >= 0 && n.x < uUniT && n.y < uUniT) {
            tile = (f * uUniT + n.y) * uUniT + n.x;
        } else {   // neighbour across a cube edge: follow the extended face plane
            vec2 uvn = (vec2(n) + 0.5) / float(uUniT) * 2.0 - 1.0;
            tile = cubeTile(normalize(cubeFaceDir(f, uvn)), uUniT);
        }
        bool dup = false;
        for (int q = 0; q < nSeen; ++q) dup = dup || (seen[q] == tile);
        if (dup) continue;
        seen[nSeen++] = tile;
        uint s0 = utile[tile], s1 = min(utile[tile + 1], s0 + 96u);
        for (uint k = s0; k < s1; ++k) {
            UStar st = ustars[k];
            vec3 delta = st.dirFlux.xyz - d;
            float d1 = dot(fp.e1, delta), d2 = dot(fp.e2, delta);
            float sp = fp.r11 * d1 + fp.r12 * d2;    // screen offset in pixels
            float sq = fp.r22 * d2;
            float e = iSaa * sp * sp + 2.0 * iSab * sp * sq + iSbb * sq * sq;
            if (!(e < 30.0)) continue;
            sum += st.color.rgb * (st.dirFlux.w * uUniStarScale * norm * exp(-0.5 * e));
        }
    }
    if (any(isnan(sum)) || any(isinf(sum))) sum = vec3(0.0);
    return min(sum, vec3(64.0));
}

vec3 sampleUniverseSky(vec3 d, vec3 a, vec3 b, float omega0) {
    vec3 col = textureGrad(uUniCube, d, a, b).rgb;
    Footprint fp = makeFootprint(d, a, b, omega0);
    return col + universeStars(d, fp, omega0);
}
#endif
