#version 460 core
// Per-pixel backward ray tracing through Schwarzschild spacetime.
#include "common.glsl"
#include "geodesic.glsl"
#include "blackbody.glsl"
#include "sky.glsl"
#include "disk.glsl"
// Two variants are compiled: BHS_BODIES 1 (planets/stars present) and 0 (none on screen), so the
// common case does not pay for the body code's register pressure.
#ifndef BHS_BODIES
#define BHS_BODIES 1
#endif
#if BHS_BODIES
#include "bodies.glsl"
#endif
#ifndef BHS_GAS
#define BHS_GAS 1
#endif
#if BHS_GAS
#include "gas.glsl"
#endif

in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform vec2  uResolution;
uniform vec2  uJitter;
uniform vec3  uCamPos;
uniform mat3  uCamBasis;        // columns: right, up, back
uniform float uTanHalfFov;
uniform int   uLensing;
uniform int   uFarMode;         // camera far from the hole: lensing is sub-pixel, rays are straight
uniform int   uMaxSteps;
uniform float uStepScale;
uniform float uDiskStep;
uniform int   uDiskMode;
uniform int   uShadowOverlay;
uniform float uPixelOmega0;     // unlensed solid angle of one internal pixel
uniform float uPixelAngle;      // angular size of one internal pixel (rad)
uniform int   uDebugView;

#if BHS_BODIES
// Composites a premultiplied (rgb, alpha) layer; returns true once the ray is fully blocked.
bool composite(vec4 layer, inout vec3 color, inout float transmittance) {
    color += transmittance * layer.rgb;
    transmittance *= 1.0 - clamp(layer.a, 0.0, 1.0);
    return transmittance < 0.004;
}

// Nearest body (sphere and rings) along a straight ray, skipping the bodies in `skipMask`.
// Positions come from relPos (camera-relative) when `cameraFrame`, else posRadius.
vec4 nearestBodyStraight(vec3 ro, vec3 rd, bool cameraFrame, uint skipMask, float tMax) {
    float bestT = tMax;
    vec4 best = vec4(0.0);
    for (int b = 0; b < uBodyCount; ++b) {
        if ((skipMask & (1u << uint(b))) != 0u) continue;
        vec3 c = cameraFrame ? bodies[b].relPos.xyz : bodies[b].posRadius.xyz;
        float tHit;
        vec4 col = traceBody(bodies[b], c, ro, rd, bestT, tHit);
        if (tHit > 0.0 && tHit < bestT) { bestT = tHit; best = col; }
    }
    return best;
}
#endif

void main() {
    vec2 px = gl_FragCoord.xy + uJitter;
    vec2 ndc = px / uResolution * 2.0 - 1.0;
    float aspect = uResolution.x / uResolution.y;
    vec3 dirCam = normalize(vec3(ndc.x * uTanHalfFov * aspect, ndc.y * uTanHalfFov, -1.0));
    vec3 dir = normalize(uCamBasis * dirCam);
#if BHS_BODIES
    gFootprint = uPixelAngle;
#endif

    float rcam = length(uCamPos);
    bool lensing = uLensing != 0 && uFarMode == 0;
    vec3 x = uCamPos;
    vec3 v = dir;
    float h2 = 0.0, h = 0.0;
    if (lensing) {
        v = initialVelocity(uCamPos, dir);
        vec3 hv = cross(x, v);
        h2 = dot(hv, hv);
        h = sqrt(h2);
    }
    float bAxis = impactParameterAboutAxis(uCamPos, dir, vec3(0.0, 1.0, 0.0));
    float rEscMin = max(uDiskOuter * 1.25, 12.0);
    // Rays with impact parameter well above b_crit never approach the photon sphere; their
    // deflection is insensitive to the step length, so they may march up to 3x coarser.
    // Near-critical rays (h < 6) keep the full uStepScale * r resolution the photon ring needs.
    float farMul = clamp((h - 6.0) / 3.0, 1.0, 3.0);   // h = 0 when lensing is off -> 1.0

    vec3 color = vec3(0.0);
    float transmittance = 1.0;
    int status = 0;            // 0 marching, 1 escaped, 2 captured, 3 opaque gas, 4 hit a body
    vec3 escDir = dir;
    int steps = 0;

    // ---- Near-field bodies. A straight ray in camera-relative coordinates is exact (to 1% of the
    // body's radius) as long as the geodesic has not bent away from it yet, and keeps close-ups of
    // tiny planets free of the precision loss of black-hole-centred coordinates. It leaves along
    // the geodesic's coordinate direction, which differs from the static observer's view direction
    // `dir` by the local aberration near the hole. Bodies entirely inside the straight stretch are
    // settled by this test (hit or miss); the coarse black-hole-centred tests must not revisit them.
    vec3 rdNear = lensing ? normalize(v) : dir;
    int nearIdx = -1;
    float nearT = 1e30;
    uint nearMask = 0u;
#if BHS_BODIES
    float aCam = lensing ? 3.0 * h2 / max(rcam * rcam * rcam * rcam, 1e-30) : 0.0;   // |x''| at the camera
    for (int b = 0; b < uBodyCount; ++b) {
        vec3 rel = bodies[b].relPos.xyz;
        float R = bodies[b].posRadius.w;
        float tStraight = lensing ? sqrt(0.02 * R / max(aCam, 1e-30)) : 1e30;
        if (length(rel) + max(R, bodies[b].ring.y) < tStraight) nearMask |= 1u << uint(b);
        float t = raySphere(vec3(0.0), rdNear, rel, R, 0.0, min(tStraight, nearT));
        if (t > 0.0) { nearT = t; nearIdx = b; }
    }
#endif

    // The march only records which body it hits; shading happens once, after the loop, so the
    // loop stays small (register pressure in the loop decides the speed of the whole tracer).
    int hitBody = -1;
    vec3 hitRo = vec3(0.0), hitRd = vec3(0.0);
    float hitT = 0.0;
    float travelled = 0.0;
    float nearTol = max(2e-6 * rcam, 1e-9);   // well above float quantisation of x near the camera
    if (uFarMode != 0) {
        status = 1;
        escDir = dir;
    } else {
        for (int i = 0; i < uMaxSteps; ++i) {
            float r = length(x);
            if (r < R_HORIZON + 0.02) { status = 2; break; }
            float vlen = length(v);
            if (nearIdx >= 0 && travelled + nearTol >= nearT) {   // reached the near-field body
                hitBody = nearIdx;
                status = 4;
                break;
            }
            if (dot(x, v) > 0.0 && r > rEscMin) {
                float b = h / vlen;
                if (!lensing || 0.75 * b * b * b / (r * r * r * r) < ESC_TOL) {
                    escDir = lensing ? farFieldDirection(x, v, h2) : normalize(v);
                    status = 1;
                    break;
                }
            }
            float ds = max(uStepScale * r * farMul, 0.02);
            float rcyl = length(x.xz);
            bool nearDisk = false;
            if (uDiskMode != DISK_NONE && rcyl > uDiskInner - 1.0 && rcyl < uDiskOuter + 1.0) {
                float outside = abs(x.y) - diskSlabHalfHeight(rcyl);      // > 0: above/below the slab
                if (outside <= 0.0) {
                    nearDisk = true;
                    if (uDiskMode == DISK_VOLUMETRIC) ds = min(ds, 6.0 * uDiskStep);   // sub-sampled below
                } else {
                    // Land on the slab boundary instead of stepping over it.
                    ds = min(ds, max(uDiskStep, outside / (abs(v.y) / vlen + 2.5 * uDiskThickness)));
                }
            }
            // Land on the near-field hit; take short steps near other bodies so the straight chord of
            // each step follows the curved ray closely where it matters.
#if BHS_BODIES
            if (nearIdx >= 0) ds = min(ds, max(nearT - travelled, nearTol));
            for (int b = 0; b < uBodyCount; ++b) {
                if ((nearMask & (1u << uint(b))) != 0u) continue;
                vec4 pr = bodies[b].posRadius;
                ds = min(ds, max(0.5 * (length(x - pr.xyz) - pr.w), 0.25 * pr.w));
            }
#endif
#if BHS_GAS
            // Gas of a star being torn apart: fine steps inside its volumes; outside, land on the
            // nearest volume instead of stepping over it.
            float gasCell = gasCellSize(x);
            if (gasCell > 0.0) ds = min(ds, max(1.5 * gasCell, 1e-6 * r));
            else {
                float te = gasEntryDistance(x, v / vlen);
                if (te < ds) ds = max(te + 1e-4 * max(r, 1.0), 1e-6 * r);
            }
#endif
            float dt = ds / vlen;
            vec3 x0 = x, v0 = v;
            if (lensing) rk4Step(x, v, h2, dt);
            else x += v * dt;
            steps = i + 1;
            float seg = length(x - x0);
            travelled += seg;

            if (uDiskMode == DISK_THIN) {
                if ((x.y > 0.0) != (x0.y > 0.0)) {
                    float f = x0.y / (x0.y - x.y);
                    vec3 hit = mix(x0, x, f);
                    vec4 s = thinDiskSample(hit, bAxis, rcam);
                    color += transmittance * s.rgb * s.a;
                    transmittance *= 1.0 - s.a;
                }
            } else if (uDiskMode == DISK_VOLUMETRIC && nearDisk) {
                // Sample the disk along the cubic Hermite interpolant of the RK4 segment so one
                // geodesic step can carry several (cheaper) disk samples.
                int n = max(1, int(ceil(seg / uDiskStep)));
                float sub = seg / float(n);
                vec3 m0 = dt * v0, m1 = dt * v;
                for (int k = 0; k < n; ++k) {
                    float t = (float(k) + 0.5) / float(n);
                    float t2 = t * t, t3 = t2 * t;
                    vec3 p = (2.0 * t3 - 3.0 * t2 + 1.0) * x0 + (t3 - 2.0 * t2 + t) * m0 +
                             (-2.0 * t3 + 3.0 * t2) * x + (t3 - t2) * m1;
                    float rm = length(p.xz);
                    if (rm > uDiskInner - 0.5 && rm < uDiskOuter && abs(p.y) < diskSlabHalfHeight(rm))
                        volumetricDiskStep(p, rm, sub, bAxis, rcam, color, transmittance);
                    if (transmittance < 0.004) break;
                }
            }
#if BHS_GAS
            {
                float cellHere = gasCell > 0.0 ? gasCell : gasCellSize(x);
                if (cellHere > 0.0 && seg > 0.0) {
                    int n = clamp(int(ceil(seg / cellHere)), 1, 8);
                    float sub = seg / float(n);
                    for (int k = 0; k < n; ++k) gasSample(mix(x0, x, (float(k) + 0.5) / float(n)), sub, color, transmittance);
                }
            }
#endif
            if (transmittance < 0.004) { status = 3; break; }

#if BHS_BODIES
            // Far-field bodies: intersect the chord of this step (spheres only; ringed planets live
            // far from the hole, where the straight-ray tests below handle them).
            if (uBodyCount > 0 && seg > 0.0) {
                vec3 sd = (x - x0) / seg;
                float bestT = seg;
                for (int b = 0; b < uBodyCount; ++b) {
                    if ((nearMask & (1u << uint(b))) != 0u) continue;
                    vec4 pr = bodies[b].posRadius;
                    float t = raySphere(x0, sd, pr.xyz, pr.w, 0.0, bestT);
                    if (t > 0.0) { bestT = t; hitBody = b; hitRo = x0; hitRd = sd; hitT = t; }
                }
                if (hitBody >= 0) { status = 4; break; }
            }
#endif
        }
        if (status == 0) status = 2;   // ran out of steps: these rays hug the photon sphere
    }

#if BHS_BODIES
    // ---- Shade the body hit by the march.
    if (status == 4 && hitBody >= 0) {
        Body bd = bodies[hitBody];
        vec3 ro = hitRo, rd = hitRd, c = bd.posRadius.xyz;
        float t = hitT;
        if (hitBody == nearIdx && travelled + nearTol >= nearT) { ro = vec3(0.0); rd = rdNear; c = bd.relPos.xyz; t = nearT; }
        vec3 n = normalize(ro + rd * t - c);
        gSurfaceLod = gFootprint * t / bd.posRadius.w / max(dot(n, -rd), 0.08);
        color += transmittance * shadeBodySurface(bd, n, rd);
        transmittance = 0.0;
    }
    // ---- Straight stretches: everything in far mode; after escape, the near-field body (which lies
    // beyond the escape point) and the other bodies along the escaped ray.
    if (status == 1) {
        vec4 layer = uFarMode != 0 ? nearestBodyStraight(vec3(0.0), dir, true, 0u, 1e30)
                                   : nearestBodyStraight(vec3(0.0), rdNear, true, ~nearMask, 1e30);
        if (uFarMode == 0 && layer.a <= 0.0) layer = nearestBodyStraight(x, escDir, false, nearMask, 1e30);
        if (composite(layer, color, transmittance)) status = 4;
    }
    // ---- Atmosphere and corona halos around near-field bodies the ray just missed.
    if (status != 4) {
        vec3 halo = vec3(0.0);
        for (int b = 0; b < uBodyCount; ++b) {
            if (uFarMode == 0 && (nearMask & (1u << uint(b))) == 0u) continue;
            vec3 rd = uFarMode != 0 ? dir : rdNear;
            halo += bodyHalo(bodies[b], bodies[b].relPos.xyz, vec3(0.0), rd, 1e30);
        }
        color += transmittance * halo;
    }
#endif

    // Sky. Derivatives must be taken in uniform control flow (after the loop).
    vec3 ddx = dFdx(escDir);
    vec3 ddy = dFdy(escDir);
    vec3 ddx0 = dFdx(dir);
    vec3 ddy0 = dFdy(dir);
    // Quads straddling the shadow edge or the opaque disk silhouette mix escaped and
    // non-escaped lanes; their derivatives are meaningless, so use the unlensed footprint there.
    float esc = float(status == 1);
    float escDx = dFdx(esc), escDy = dFdy(esc);
    if (escDx != 0.0 || escDy != 0.0) { ddx = ddx0; ddy = ddy0; }
    vec3 sky = vec3(0.0);
    if (status == 1) sky = sampleSky(escDir, ddx, ddy, uPixelOmega0, rcam);
    color += transmittance * sky;

    if (uShadowOverlay != 0 && uFarMode == 0) {
        float ang = acos(clamp(dot(dir, -uCamPos / rcam), -1.0, 1.0));
        float edge = shadowAngularRadius(rcam);
        float d = abs(ang - edge) / uPixelAngle;
        color = mix(color, vec3(0.1, 1.0, 0.2), 0.85 * (1.0 - smoothstep(0.8, 1.8, d)));
    }
    if (uDebugView == 1) {
        color = heatmap(float(steps) / float(uMaxSteps));
        if (status == 2) color *= 0.35;
    } else if (uDebugView == 3) {
        // NaN detector: red = non-finite escape direction / state, green = non-finite sky sample,
        // blue = non-finite derivative, grey = fine (brightness = status).
        bool badDir = any(isnan(escDir)) || any(isinf(escDir)) || any(isnan(x)) || any(isnan(v));
        bool badSky = any(isnan(sky)) || any(isinf(sky));
        bool badDer = any(isnan(ddx)) || any(isnan(ddy)) || any(isinf(ddx)) || any(isinf(ddy));
        color = vec3(0.15 * float(status));
        if (badDer) color = vec3(0.0, 0.0, 1.0);
        if (badSky) color = vec3(0.0, 1.0, 0.0);
        if (badDir) color = vec3(1.0, 0.0, 0.0);
    } else if (uDebugView == 2) {
        float omega = sqrt(max(dot(ddx, ddx) * dot(ddy, ddy) - dot(ddx, ddy) * dot(ddx, ddy), 1e-30));
        color = (status == 1) ? heatmap(0.5 + 0.12 * log2(omega / uPixelOmega0)) : vec3(0.0);
    }
    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    fragColor = vec4(max(color, 0.0), 1.0);
}
