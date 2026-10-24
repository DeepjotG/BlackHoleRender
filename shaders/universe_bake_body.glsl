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
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= uRes || px.y >= uRes) return;
    vec2 uv = (vec2(px) + 0.5) / float(uRes) * 2.0 - 1.0;
    vec3 dr = normalize(cubeFaceDir(uFace, uv));
    vec3 dg = normalize(uRt * dr);

    vec3 L = vec3(0.0);
    float tau = 0.0;
    for (int gi = 0; gi < uNearCount; ++gi) {
        vec3 c = galaxyCenterKpc(nearGal[gi]);
        float Rb = galaxyBoundRadiusKpc(nearGal[gi]);
        float b = dot(c, dg);
        float disc = b * b - (dot(c, c) - Rb * Rb);
        if (disc <= 0.0) continue;
        float s = sqrt(disc);
        float t0 = max(b - s, 0.0), t1 = b + s;
        if (t1 <= 0.0) continue;
        const int N = 72;
        float prevT = t0;
        for (int k = 1; k <= N; ++k) {
            float f = float(k) / float(N);
            float t = t0 + (t1 - t0) * f * f;          // denser sampling close to the viewer
            float tm = 0.5 * (prevT + t);
            float dtPc = (t - prevT) * 1000.0;
            prevT = t;
            vec3 emis;
            float ext;
            galaxySample(nearGal[gi], dg * tm, emis, ext);
            float unresolved = 1.0 - exp(-tm * 1000.0 / uResolvedPc);
            L += exp(-tau) * max(emis, vec3(0.0)) * (dtPc * unresolved);
            tau += max(ext, 0.0) * dtPc;
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
    col += sprite * exp(-min(tau, 30.0));

    if (any(isnan(col)) || any(isinf(col))) col = vec3(0.0);
    imageStore(uOut, ivec3(px, uFace), vec4(max(col, vec3(0.0)), min(tau, 60000.0)));
}
