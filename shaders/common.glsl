#ifndef COMMON_GLSL
#define COMMON_GLSL
// Shared constants and helpers. Units: G = c = M = 1.

const float PI = 3.14159265358979;
const float TWO_PI = 6.28318530717959;
const float R_HORIZON = 2.0;
const float R_PHOTON = 3.0;
const float R_ISCO = 6.0;
const float B_CRIT = 5.19615242270663;   // 3*sqrt(3): critical impact parameter

const int DISK_NONE = 0;
const int DISK_THIN = 1;
const int DISK_VOLUMETRIC = 2;

// Uniforms shared by several include files.
uniform float uTime;
uniform float uDopplerStrength;

// ---------------------------------------------------------------- hashing
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
vec3 hash3u(uvec3 p) { return vec3(pcg3d(p)) * (1.0 / 4294967296.0); }
float hash1u(uint p) { return float(pcg(p)) * (1.0 / 4294967296.0); }
float hash12(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)));
    return hash1u(q.x * 1973u + q.y * 9277u + 26699u);
}

// ---------------------------------------------------------------- gradient noise
vec3 grad3(ivec3 c, uint salt) {
    vec3 h = hash3u(uvec3(c) + uvec3(salt, salt * 3u + 11u, salt * 7u + 29u));
    vec3 g = h * 2.0 - 1.0;
    return g * inversesqrt(max(dot(g, g), 1e-8));
}
// 3D Perlin-style gradient noise, range roughly [-1, 1].
float gnoise3(vec3 p, uint salt) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec3 c = ivec3(i);
    float n000 = dot(grad3(c + ivec3(0, 0, 0), salt), f - vec3(0, 0, 0));
    float n100 = dot(grad3(c + ivec3(1, 0, 0), salt), f - vec3(1, 0, 0));
    float n010 = dot(grad3(c + ivec3(0, 1, 0), salt), f - vec3(0, 1, 0));
    float n110 = dot(grad3(c + ivec3(1, 1, 0), salt), f - vec3(1, 1, 0));
    float n001 = dot(grad3(c + ivec3(0, 0, 1), salt), f - vec3(0, 0, 1));
    float n101 = dot(grad3(c + ivec3(1, 0, 1), salt), f - vec3(1, 0, 1));
    float n011 = dot(grad3(c + ivec3(0, 1, 1), salt), f - vec3(0, 1, 1));
    float n111 = dot(grad3(c + ivec3(1, 1, 1), salt), f - vec3(1, 1, 1));
    float nx00 = mix(n000, n100, u.x), nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x), nx11 = mix(n011, n111, u.x);
    return 1.4 * mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z);
}
// Periodic variant: lattice wraps every `period` cells on each axis (tileable textures).
float pgnoise3(vec3 p, int period, uint salt) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec3 c = ivec3(i);
    ivec3 c0 = ((c % period) + period) % period;
    ivec3 c1 = ((c + 1) % period + period) % period;
    float n000 = dot(grad3(ivec3(c0.x, c0.y, c0.z), salt), f - vec3(0, 0, 0));
    float n100 = dot(grad3(ivec3(c1.x, c0.y, c0.z), salt), f - vec3(1, 0, 0));
    float n010 = dot(grad3(ivec3(c0.x, c1.y, c0.z), salt), f - vec3(0, 1, 0));
    float n110 = dot(grad3(ivec3(c1.x, c1.y, c0.z), salt), f - vec3(1, 1, 0));
    float n001 = dot(grad3(ivec3(c0.x, c0.y, c1.z), salt), f - vec3(0, 0, 1));
    float n101 = dot(grad3(ivec3(c1.x, c0.y, c1.z), salt), f - vec3(1, 0, 1));
    float n011 = dot(grad3(ivec3(c0.x, c1.y, c1.z), salt), f - vec3(0, 1, 1));
    float n111 = dot(grad3(ivec3(c1.x, c1.y, c1.z), salt), f - vec3(1, 1, 1));
    float nx00 = mix(n000, n100, u.x), nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x), nx11 = mix(n011, n111, u.x);
    return 1.4 * mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z);
}
float fbm3(vec3 p, int octaves, float gain, uint salt) {
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gnoise3(p, salt + uint(i) * 101u);
        norm += amp;
        amp *= gain;
        p = p * 2.02 + vec3(17.3, 9.1, 31.7);
    }
    return sum / norm;
}

// ---------------------------------------------------------------- colour helpers
vec3 heatmap(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 * t, 1.5 * (1.0 - abs(t - 0.5) * 2.0), 1.5 * (1.0 - t)) , 0.0, 1.0);
}
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

#endif
