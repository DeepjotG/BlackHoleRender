#ifndef BODIES_GLSL
#define BODIES_GLSL
#include "common.glsl"
#include "blackbody.glsl"
// Spheres in the scene: planets (lit, procedural surfaces, atmospheres, rings) and stars
// (emissive). Filled each frame by the app (src/Bodies.cpp). Positions:
//   posRadius.xyz: centre relative to the black hole, renderer frame (M), used while marching
//   relPos.xyz:    centre relative to the camera (M), precise even for tiny bodies close by

struct Body {
    vec4 posRadius;   // xyz centre (black-hole relative), w radius (M)
    vec4 relPos;      // xyz centre (camera relative), w kind (0 planet, 1 star)
    vec4 look;        // x look id, y seed, z atmosphere strength, w rotation angle (rad)
    vec4 tint;        // rgb albedo tint
    vec4 light;       // xyz unit direction from the body towards its light, w intensity
    vec4 lightColor;  // rgb light colour; a = effective temperature for stars (K)
    vec4 axis;        // xyz unit spin / ring axis
    vec4 ring;        // x inner radius, y outer radius (M; 0 = no rings)
};
layout(std430, binding = 3) readonly buffer BodyBuffer { Body bodies[]; };
uniform int uBodyCount;
// Pixel footprint: set to radians per pixel before tracing; traceBody turns it into the footprint
// on the surface in units of the body radius, which planetSurface uses to fade detail that would alias.
float gFootprint = 0.001;
float gSurfaceLod = 0.0;
float lodFade(float frequency) { return 1.0 - smoothstep(0.25, 1.0, frequency * gSurfaceLod); }

const int LOOK_ROCKY = 0, LOOK_DESERT = 1, LOOK_OCEAN = 2, LOOK_ICE = 3, LOOK_EARTH = 4,
          LOOK_GAS_WARM = 5, LOOK_GAS_COLD = 6, LOOK_CLOUDY = 7, LOOK_LAVA = 8;

// Nearest intersection of a ray with a sphere; returns -1 if none in (tMin, tMax).
float raySphere(vec3 ro, vec3 rd, vec3 c, float r, float tMin, float tMax) {
    vec3 oc = ro - c;
    float b = dot(oc, rd);
    float cc = dot(oc, oc) - r * r;
    float disc = b * b - cc;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t = -b - s;
    if (t <= tMin) t = -b + s;            // inside the sphere
    return (t > tMin && t < tMax) ? t : -1.0;
}

// Body-local coordinates of a unit normal: rotate about the spin axis so surfaces turn.
vec3 bodyLocal(Body b, vec3 n) {
    vec3 ax = normalize(b.axis.xyz);
    vec3 ref = abs(ax.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 e1 = normalize(cross(ref, ax));
    vec3 e2 = cross(ax, e1);
    float ca = cos(b.look.w), sa = sin(b.look.w);
    vec3 u = e1 * ca + e2 * sa, v = -e1 * sa + e2 * ca;
    return vec3(dot(n, u), dot(n, ax), dot(n, v));   // y = along the axis (latitude)
}

float ridged(vec3 p, int oct, uint salt) {
    float s = 0.0, a = 0.5, nrm = 0.0;
    for (int i = 0; i < oct; ++i) {
        s += a * (1.0 - abs(gnoise3(p, salt + uint(i) * 17u)));
        nrm += a;
        a *= 0.5;
        p = p * 2.03 + 7.1;
    }
    return s / nrm;
}

// Crater field: returns 0 (none) .. 1 (crater floor) and a rim term.
vec2 craters(vec3 p, float scale, uint salt) {
    vec3 q = p * scale;
    vec3 i = floor(q);
    vec2 best = vec2(0.0);
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        vec3 c = i + vec3(x, y, z);
        vec3 h = hash3u(uvec3(ivec3(c)) + uvec3(salt, salt * 3u, salt * 7u));
        if (h.x > 0.45) continue;
        vec3 center = c + h;
        float rad = 0.15 + 0.35 * h.y;
        float d = length(q - center) / rad;
        if (d < 1.25) {
            float floorv = 1.0 - smoothstep(0.7, 1.0, d);
            float rim = smoothstep(0.75, 1.0, d) * (1.0 - smoothstep(1.0, 1.25, d));
            best = max(best, vec2(floorv, rim));
        }
    }
    return best;
}

vec3 lookAtmoColor(int look) {
    if (look == LOOK_OCEAN) return vec3(0.35, 0.6, 1.0);
    if (look == LOOK_ICE) return vec3(0.6, 0.78, 1.0);
    if (look == LOOK_DESERT) return vec3(1.0, 0.62, 0.42);
    if (look == LOOK_ROCKY) return vec3(0.0);
    if (look == LOOK_EARTH) return vec3(0.35, 0.58, 1.0);
    if (look == LOOK_CLOUDY) return vec3(1.0, 0.88, 0.6);
    if (look == LOOK_GAS_WARM) return vec3(1.0, 0.85, 0.65);
    if (look == LOOK_GAS_COLD) return vec3(0.55, 0.85, 1.0);
    return vec3(1.0, 0.45, 0.2);
}

struct Surface {
    vec3 albedo;
    vec3 emission;
    float specular;   // 0..1
    float roughness;  // 0..1
    vec3 atmoColor;
};

Surface planetSurface(Body b, vec3 n) {
    vec3 p = bodyLocal(b, n);
    int look = int(b.look.x + 0.5);
    uint seed = uint(b.look.y);
    float lat = p.y;
    Surface s;
    s.emission = vec3(0.0);
    s.specular = 0.0;
    s.roughness = 0.8;
    s.atmoColor = vec3(0.4, 0.6, 1.0);
    if (look == LOOK_OCEAN) {
        // Deep ocean with enormous tidal swells (Miller's planet), foam and a few clouds.
        // Swells run around the planet perpendicular to the tidal axis (local x), bent by noise.
        float swell = gnoise3(vec3(p.x * 2.0, p.y * 6.0, p.z * 2.0) + float(seed), seed);
        float bands = 0.5 + 0.5 * sin(p.x * 14.0 + 3.0 * swell) * lodFade(14.0 / 6.283);
        float detail = fbm3(p * 9.0, 4, 0.5, seed + 3u) * lodFade(9.0);
        vec3 deep = vec3(0.03, 0.10, 0.15), crest = vec3(0.16, 0.36, 0.42);
        s.albedo = mix(deep, crest, smoothstep(0.55, 0.95, bands) * 0.8 + 0.2 * detail);
        float foam = smoothstep(0.9, 0.99, bands) * smoothstep(0.0, 0.5, detail) * lodFade(20.0);
        s.albedo = mix(s.albedo, vec3(0.8), foam * 0.6);
        float cloud = smoothstep(0.25, 0.7, fbm3(p * 4.0 + 13.0, 5, 0.55, seed + 9u));
        s.albedo = mix(s.albedo, vec3(0.9), cloud * 0.55);
        s.specular = 0.8 * (1.0 - cloud);
        s.roughness = 0.25;
        s.atmoColor = vec3(0.35, 0.6, 1.0);
    } else if (look == LOOK_ICE) {
        float base = fbm3(p * 3.0, 5, 0.5, seed);
        float cracks = ridged(p * 6.0, 4, seed + 5u);
        vec3 ice = mix(vec3(0.78, 0.84, 0.9), vec3(0.95, 0.97, 1.0), 0.5 + 0.5 * base);
        s.albedo = mix(ice, vec3(0.42, 0.55, 0.68), smoothstep(0.86, 0.97, cracks));
        float cloud = smoothstep(0.35, 0.8, fbm3(p * 5.0 + 3.0, 4, 0.5, seed + 2u));
        s.albedo = mix(s.albedo, vec3(0.97), cloud * 0.4);
        s.specular = 0.3;
        s.roughness = 0.5;
        s.atmoColor = vec3(0.6, 0.78, 1.0);
    } else if (look == LOOK_DESERT) {
        float big = fbm3(p * 2.5, 5, 0.55, seed);
        float dunes = 0.5 + 0.5 * sin(dot(p, vec3(40.0, 13.0, 27.0)) + 8.0 * fbm3(p * 6.0, 3, 0.5, seed + 1u)) * lodFade(8.0);
        vec3 sand = mix(vec3(0.62, 0.40, 0.24), vec3(0.86, 0.66, 0.44), 0.5 + 0.5 * big);
        s.albedo = sand * (0.85 + 0.15 * dunes);
        vec2 cr = craters(p, 7.0, seed + 4u);
        s.albedo *= 1.0 - 0.25 * cr.x + 0.15 * cr.y;
        s.albedo = mix(s.albedo, vec3(0.92, 0.9, 0.88), smoothstep(0.82, 0.92, abs(lat)));   // polar frost
        s.atmoColor = vec3(1.0, 0.62, 0.42);
    } else if (look == LOOK_ROCKY) {
        float base = fbm3(p * 3.0, 5, 0.5, seed);
        s.albedo = vec3(0.32, 0.30, 0.29) * (0.8 + 0.4 * base);
        vec2 c1 = craters(p, 6.0, seed + 1u), c2 = craters(p, 17.0, seed + 2u);
        s.albedo *= 1.0 - 0.3 * max(c1.x, c2.x) + 0.25 * max(c1.y, c2.y);
        s.atmoColor = vec3(0.0);
    } else if (look == LOOK_EARTH) {
        float land = fbm3(p * 2.2, 6, 0.55, seed);
        float coast = smoothstep(0.02, 0.08, land);
        vec3 ocean = vec3(0.015, 0.05, 0.13);
        float dry = fbm3(p * 4.0 + 17.0, 4, 0.5, seed + 1u);
        vec3 ground = mix(vec3(0.10, 0.22, 0.07), vec3(0.42, 0.35, 0.22), smoothstep(-0.2, 0.4, dry + 0.6 * (1.0 - abs(lat) * 1.6)));
        s.albedo = mix(ocean, ground, coast);
        s.albedo = mix(s.albedo, vec3(0.92, 0.94, 0.97), smoothstep(0.78, 0.86, abs(lat) + 0.08 * land));
        float cloud = smoothstep(0.1, 0.6, fbm3(p * 3.5 + 5.0, 6, 0.55, seed + 8u));
        s.albedo = mix(s.albedo, vec3(0.95), cloud * 0.8);
        s.specular = 0.7 * (1.0 - coast) * (1.0 - cloud);
        s.roughness = 0.3;
        s.atmoColor = vec3(0.35, 0.58, 1.0);
    } else if (look == LOOK_CLOUDY) {
        float w = fbm3(p * 3.0, 5, 0.5, seed);
        float bands = 0.5 + 0.5 * sin(lat * 9.0 + 3.0 * w);
        s.albedo = mix(vec3(0.80, 0.72, 0.52), vec3(0.95, 0.90, 0.75), bands);
        s.atmoColor = vec3(1.0, 0.88, 0.6);
    } else if (look == LOOK_GAS_WARM || look == LOOK_GAS_COLD) {
        vec3 q = p + 0.12 * vec3(fbm3(p * 3.0, 4, 0.5, seed), 0.0, fbm3(p * 3.0 + 9.0, 4, 0.5, seed + 1u));
        float turb = fbm3(vec3(q.x * 6.0, q.y * 30.0, q.z * 6.0), 5, 0.55, seed + 2u);
        float bands = 0.5 + 0.5 * sin(q.y * 26.0 + 2.5 * turb) * lodFade(26.0 / 6.283);
        if (look == LOOK_GAS_WARM) {
            s.albedo = mix(vec3(0.62, 0.42, 0.26), vec3(0.93, 0.86, 0.74), bands);
            // A great storm.
            float spot = 1.0 - smoothstep(0.08, 0.16, length(vec2(p.x - 0.6, (p.y + 0.35) * 1.8)));
            s.albedo = mix(s.albedo, vec3(0.72, 0.32, 0.20), spot * 0.8);
            s.atmoColor = vec3(1.0, 0.85, 0.65);
        } else {
            s.albedo = mix(vec3(0.42, 0.70, 0.80), vec3(0.62, 0.86, 0.92), bands * 0.35 + 0.3 * turb);
            s.atmoColor = vec3(0.55, 0.85, 1.0);
        }
        s.roughness = 0.9;
    } else {  // LOOK_LAVA
        float crust = fbm3(p * 3.0, 5, 0.5, seed);
        float cracks = ridged(p * 5.0, 5, seed + 3u);
        s.albedo = vec3(0.06, 0.05, 0.05) * (0.7 + 0.6 * crust);
        float glow = smoothstep(0.82, 0.96, cracks);
        s.emission = vec3(1.0, 0.35, 0.05) * glow * 2.5;
        s.atmoColor = vec3(1.0, 0.45, 0.2);
    }
    s.albedo *= b.tint.rgb;
    return s;
}

// Radiance leaving a body's surface towards the viewer. n = outward normal at the hit,
// viewDir = direction of the incoming ray (camera -> hit).
vec3 shadeBodySurface(Body b, vec3 n, vec3 viewDir) {
    if (b.relPos.w > 0.5) {
        // Star: limb-darkened blackbody photosphere with granulation.
        float mu = max(dot(n, -viewDir), 0.0);
        float limb = 0.4 + 0.6 * pow(mu, 0.6);
        vec3 p = bodyLocal(b, n);
        float gran = 0.92 + 0.16 * fbm3(p * 60.0, 3, 0.5, uint(b.look.y));
        vec4 bb = blackbody(b.lightColor.a);
        return bb.rgb * (b.light.w * limb * gran);
    }
    Surface s = planetSurface(b, n);
    vec3 L = normalize(b.light.xyz);
    float ndl = dot(n, L);
    float atmo = b.look.z;
    // Gentle wrap lighting for planets with air (scattering softens the terminator).
    float wrap = mix(max(ndl, 0.0), clamp((ndl + 0.25) / 1.25, 0.0, 1.0), clamp(atmo, 0.0, 1.0));
    vec3 irr = b.lightColor.rgb * b.light.w;
    vec3 col = s.albedo * irr * wrap + s.albedo * irr * 0.015 + s.emission;
    if (s.specular > 0.0 && ndl > 0.0) {
        vec3 h = normalize(L - viewDir);
        float shininess = mix(400.0, 20.0, s.roughness);
        col += irr * s.specular * pow(max(dot(n, h), 0.0), shininess) * ndl * 0.6;
    }
    // Atmospheric rim scattering on the lit side.
    if (atmo > 0.0) {
        float rim = pow(1.0 - max(dot(n, -viewDir), 0.0), 3.0);
        col += s.atmoColor * irr * rim * atmo * 0.6 * clamp(ndl + 0.35, 0.0, 1.0);
    }
    return col;
}

// Ring plane hit for a ray (in the same frame as `c`): returns colour (rgb) and opacity (a).
vec4 ringSample(Body b, vec3 c, vec3 ro, vec3 rd, float tMax, out float tRing) {
    tRing = -1.0;
    if (b.ring.y <= 0.0) return vec4(0.0);
    vec3 ax = normalize(b.axis.xyz);
    float denom = dot(rd, ax);
    if (abs(denom) < 1e-6) return vec4(0.0);
    float t = dot(c - ro, ax) / denom;
    if (t <= 0.0 || t >= tMax) return vec4(0.0);
    vec3 q = ro + rd * t - c;
    float rr = length(q);
    if (rr < b.ring.x || rr > b.ring.y) return vec4(0.0);
    float u = (rr - b.ring.x) / (b.ring.y - b.ring.x);
    float dens = 0.55 + 0.45 * gnoise3(vec3(u * 60.0, 0.0, 0.0), 77u);
    dens *= smoothstep(0.0, 0.04, u) * (1.0 - smoothstep(0.96, 1.0, u));
    dens *= 1.0 - 0.85 * (1.0 - smoothstep(0.55, 0.57, u)) * smoothstep(0.52, 0.54, u);   // a gap
    vec3 L = normalize(b.light.xyz);
    // Planet shadow on the rings.
    float sh = raySphere(q, L, vec3(0.0), b.posRadius.w, 0.0, 1e30) > 0.0 ? 0.1 : 1.0;
    vec3 col = vec3(0.80, 0.72, 0.58) * b.tint.rgb * b.lightColor.rgb * b.light.w * (0.3 + 0.7 * abs(dot(L, ax))) * sh;
    tRing = t;
    return vec4(col, clamp(dens, 0.0, 0.95));
}

// Full body (sphere + rings) along a straight ray segment; returns rgb radiance and alpha (1 if the
// sphere was hit). `c` is the body centre in the ray's frame. tHit receives the nearest hit.
vec4 traceBody(Body b, vec3 c, vec3 ro, vec3 rd, float tMax, out float tHit) {
    float R = b.posRadius.w;
    float ts = raySphere(ro, rd, c, R, 0.0, tMax);
    float tr;
    vec4 ring = ringSample(b, c, ro, rd, ts > 0.0 ? ts : tMax, tr);
    vec4 result = vec4(0.0);
    tHit = -1.0;
    if (ts > 0.0) {
        vec3 n = normalize(ro + rd * ts - c);
        gSurfaceLod = gFootprint * ts / R / max(dot(n, -rd), 0.08);
        result = vec4(shadeBodySurface(b, n, rd), 1.0);
        tHit = ts;
    }
    if (tr > 0.0) {
        // Ring in front of the sphere (or no sphere hit): composite over.
        result.rgb = ring.rgb * ring.a + result.rgb * (1.0 - ring.a);
        result.a = ring.a + result.a * (1.0 - ring.a);
        tHit = tHit > 0.0 ? min(tHit, tr) : tr;
    }
    return result;
}

// Thin glow around stars and airy planets for rays that just miss them.
vec3 bodyHalo(Body b, vec3 c, vec3 ro, vec3 rd, float tMax) {
    float R = b.posRadius.w;
    vec3 oc = c - ro;
    float t = clamp(dot(oc, rd), 0.0, tMax);
    float d = length(ro + rd * t - c);
    if (d < R) return vec3(0.0);
    float x = (d - R) / R;
    if (b.relPos.w > 0.5) {
        vec4 bb = blackbody(b.lightColor.a);
        return bb.rgb * b.light.w * 0.25 * exp(-x * 6.0);
    }
    float atmo = b.look.z;
    if (atmo <= 0.0) return vec3(0.0);
    vec3 L = normalize(b.light.xyz);
    vec3 dirToPoint = normalize(ro + rd * t - c);
    float lit = clamp(dot(dirToPoint, L) + 0.3, 0.0, 1.0);
    return lookAtmoColor(int(b.look.x + 0.5)) * b.lightColor.rgb * b.light.w * atmo * lit * 0.5 * exp(-x * 40.0);
}
#endif
