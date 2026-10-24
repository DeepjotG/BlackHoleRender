// Body of the diffuse-sky bake (included after a galaxy model providing GalaxyGpu, galaxySample,
// galaxyCenterKpc and galaxyBoundRadiusKpc). One cube face per dispatch, from the camera:
//   * nearby galaxies (including the host) are ray-marched as volumes: unresolved starlight with
//     dust extinction; light from close by is faded out because those stars are drawn individually;
//   * distant galaxies are procedural images (sprites), dimmed by the accumulated dust.
// Output: rgb = radiance (HDR units), a = V-band optical depth to infinity (used to dim stars).
#include "common.glsl"
#include "universe_common.glsl"

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
layout(rgba16f, binding = 0) uniform writeonly imageCube uOut;

uniform int uRes;
uniform int uFace;
uniform int uRowOffset;        // first row of the strip baked by this dispatch
uniform float uDustScale;      // 1 (debug: BHS_DUST_SCALE)
uniform mat3 uRt;              // renderer -> galactic rotation
uniform int uNearCount;
uniform float uScale;          // (L_sun / pc^2) -> HDR radiance, including 1 / (4 pi)
uniform float uResolvedPc;     // distance scale over which starlight becomes unresolved
uniform int uSpriteGrid;       // sprite tiles per face edge

layout(std430, binding = 0) readonly buffer NearGalaxies { GalaxyGpu nearGal[]; };
struct Sprite {
    vec4 dirCos;   // xyz: direction (renderer frame), w: cos(angular radius)
    vec4 axisQ;    // xyz: apparent major axis (renderer frame), w: axis ratio
    vec4 color;    // rgb: tint, a: peak radiance (HDR)
    vec4 params;   // x: type (0 elliptical-like, 1 spiral, 2 barred), y: arms, z: pitch, w: seed
};
layout(std430, binding = 1) readonly buffer Sprites { Sprite sprites[]; };
layout(std430, binding = 2) readonly buffer SpriteTiles { uint spriteRange[]; };
layout(std430, binding = 3) readonly buffer SpriteIdx { uint spriteIdx[]; };

float ubSq(float x) { return x * x; }

float ubSersic(float rho, float re, float n) {
    float bn = 2.0 * n - 1.0 / 3.0;
    return exp(-bn * (pow(max(rho, 1e-5) / re, 1.0 / n) - 1.0));
}

vec3 spriteRadiance(Sprite g, vec3 d) {
    vec3 c = g.dirCos.xyz;
    float cosA = dot(d, c);
    if (cosA < g.dirCos.w) return vec3(0.0);
    float R = acos(clamp(g.dirCos.w, -1.0, 1.0));
    vec3 off = d / max(cosA, 1e-4) - c;
    vec3 u = g.axisQ.xyz;
    vec3 w = cross(c, u);
    float tanR = tan(max(R, 1e-7));
    float ex = dot(off, u) / tanR;
    float ey = dot(off, w) / (tanR * max(g.axisQ.w, 0.05));
    float rho = sqrt(ex * ex + ey * ey);
    if (rho > 1.0) return vec3(0.0);
    float type = g.params.x, seed = g.params.w;
    vec3 col;
    if (type < 0.5) {
        col = g.color.rgb * ubSersic(rho, 0.22, 4.0);
    } else {
        float bulge = ubSersic(rho, 0.07, 2.5) * 0.9;
        float disk = exp(-rho / 0.28);
        float theta = atan(ey, ex);
        float phase = log(max(rho, 1e-3)) / max(g.params.z, 0.05);
        float armPattern = pow(0.5 + 0.5 * cos(g.params.y * (theta - phase) + seed), 3.0) * smoothstep(0.03, 0.15, rho);
        float n1 = gnoise3(vec3(ex, ey, seed) * 9.0, 31u);
        float armMod = mix(0.3, 1.0, armPattern) * (0.85 + 0.45 * n1);
        vec3 diskCol = mix(g.color.rgb, vec3(0.62, 0.74, 1.0), 0.55 * armPattern);
        col = vec3(1.0, 0.86, 0.62) * bulge + diskCol * armMod * disk;
        if (type > 1.5) col += vec3(1.0, 0.88, 0.68) * exp(-ubSq(ey / 0.06)) * exp(-ubSq(ex / 0.3)) * 0.7;
    }
    col *= 1.0 - smoothstep(0.75, 1.0, rho);
    return col * g.color.a;
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy) + ivec2(0, uRowOffset);
    if (px.x >= uRes || px.y >= uRes) return;
    vec2 uv = (vec2(px) + 0.5) / float(uRes) * 2.0 - 1.0;
    vec3 dr = normalize(cubeFaceDir(uFace, uv));
    vec3 dg = normalize(uRt * dr);

    // Nearby galaxies (sorted front to back on the CPU), ray-marched with adaptive steps: fine close to
    // the viewer, close to each galaxy's centre and where the ray crosses its thin dust layer; detail
    // smaller than a step (or than a texel) is averaged out by the model's level of detail.
    // Extinction relative to V for the R, G and B channels (Cardelli et al. 1989, R_V = 3.1).
    const vec3 kRedden = vec3(0.8, 1.0, 1.25);
    vec3 L = vec3(0.0);
    float tau = 0.0;   // V band
    float texelAngle = 2.0 / float(uRes);
    float jitter = hash12(vec2(px) + vec2(float(uFace) * 1031.0, 17.0));
    for (int gi = 0; gi < uNearCount && tau < 30.0; ++gi) {
        vec3 c = galaxyCenterKpc(nearGal[gi]);
        float Rb = galaxyBoundRadiusKpc(nearGal[gi]);
        float b = dot(c, dg);
        float disc = b * b - (dot(c, c) - Rb * Rb);
        if (disc <= 0.0) continue;
        float s = sqrt(disc);
        float t0 = max(b - s, 0.0), t1 = b + s;
        if (t1 <= 0.0) continue;
        vec3 nrm = galaxyNormal(nearGal[gi]);
        float thin = galaxyThinKpc(nearGal[gi]);
        float cosn = max(abs(dot(dg, nrm)), 1e-3);
        float minStep = max((t1 - t0) * 2e-4, 0.0008);
        // Disks: once a ray heads away from the midplane beyond 8 scale heights (and outside the
        // bulge) nothing is left to see in this galaxy.
        bool isDisk = abs(nearGal[gi].data[0].w - 3.0) > 0.5;
        float exitY = 4.0 / nearGal[gi].data[3].w;              // 8 scale heights (data[3].w = 1 / (2 hz))
        float exitR = 12.0 * nearGal[gi].data[6].w;             // 12 bulge scale radii
        if (nearGal[gi].data[14].z > 0.0) exitR = max(exitR, 1.4 / nearGal[gi].data[14].z);   // and the bar
        float t = max(t0, 0.02);   // nothing to see inside the viewer's dust-free bubble
        for (int k = 0; k < 170 && t < t1; ++k) {
            vec3 dc = dg * t - c;
            float yc = dot(dc, nrm);
            if (isDisk && abs(yc) > exitY && yc * dot(dg, nrm) > 0.0 && length(dc) > exitR) break;
            float st = 0.09 * max(min(t, length(dc)), 0.004);
            st = min(st, 0.35 * max(abs(dot(dc, nrm)), thin) / cosn);
            st = max(st, minStep);
            if (k == 0) st *= 0.05 + jitter;      // per-texel offset of the whole sample sequence
            st = min(st, t1 - t);
            float tm = t + 0.5 * st;
            vec3 light, neb;
            float ext;
            galaxySampleLod(nearGal[gi], dg * tm, max(2.0 * st, tm * texelAngle), light, neb, ext);
            float dtPc = st * 1000.0;
            // The viewer sits in a small local bubble (like the Sun's): no dust within ~50 pc and
            // nebulae faded within a few hundred pc, so being inside a cloud or an HII region never
            // fogs the whole sky; farther ones show as distinct dark lanes and pink patches.
            float dTau = max(ext, 0.0) * dtPc * smoothstep(0.02, 0.08, tm) * uDustScale;
            // Stars close by are drawn individually, so their share of the diffuse light fades out
            // near the viewer.
            float unresolved = 1.0 - exp(-tm * 1000.0 / uResolvedPc);
            vec3 src = max(light, vec3(0.0)) * unresolved + max(neb, vec3(0.0)) * (1.0 - exp(-tm / 0.5));
            vec3 dT = dTau * kRedden;
            vec3 through = mix(1.0 - 0.5 * dT, (1.0 - exp(-dT)) / max(dT, vec3(1e-6)), step(vec3(1e-4), dT));
            L += exp(-tau * kRedden) * src * (dtPc * through);
            tau += dTau;
            t += st;
            if (tau > 30.0) break;
        }
    }
    vec3 col = L * uScale;

    // Distant galaxies behind whatever dust we have accumulated.
    int f;
    vec2 fuv;
    cubeFaceUV(dr, f, fuv);
    ivec2 tij = clamp(ivec2((fuv * 0.5 + 0.5) * float(uSpriteGrid)), ivec2(0), ivec2(uSpriteGrid - 1));
    int tile = (f * uSpriteGrid + tij.y) * uSpriteGrid + tij.x;
    uint start = spriteRange[tile * 2 + 0], count = spriteRange[tile * 2 + 1];
    vec3 sprite = vec3(0.0);
    for (uint k = 0u; k < count; ++k) sprite += spriteRadiance(sprites[spriteIdx[start + k]], dr);
    col += sprite * exp(-min(tau, 30.0) * kRedden);

    if (any(isnan(col)) || any(isinf(col))) col = vec3(0.0);
    imageStore(uOut, ivec3(px, uFace), vec4(max(col, vec3(0.0)), min(tau, 60000.0)));
}
