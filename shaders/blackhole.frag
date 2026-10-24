#version 460 core
// Per-pixel backward ray tracing through Schwarzschild spacetime.
#include "common.glsl"
#include "geodesic.glsl"
#include "blackbody.glsl"
#include "sky.glsl"
#include "disk.glsl"

in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform vec2  uResolution;
uniform vec2  uJitter;
uniform vec3  uCamPos;
uniform mat3  uCamBasis;        // columns: right, up, back
uniform float uTanHalfFov;
uniform int   uLensing;
uniform int   uMaxSteps;
uniform float uStepScale;
uniform float uDiskStep;
uniform int   uDiskMode;
uniform int   uShadowOverlay;
uniform float uPixelOmega0;     // unlensed solid angle of one internal pixel
uniform float uPixelAngle;      // angular size of one internal pixel (rad)
uniform int   uDebugView;

void main() {
    vec2 px = gl_FragCoord.xy + uJitter;
    vec2 ndc = px / uResolution * 2.0 - 1.0;
    float aspect = uResolution.x / uResolution.y;
    vec3 dirCam = normalize(vec3(ndc.x * uTanHalfFov * aspect, ndc.y * uTanHalfFov, -1.0));
    vec3 dir = normalize(uCamBasis * dirCam);

    float rcam = length(uCamPos);
    vec3 x = uCamPos;
    vec3 v = dir;
    float h2 = 0.0, h = 0.0;
    if (uLensing != 0) {
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
    int status = 0;            // 0 marching, 1 escaped, 2 captured, 3 opaque
    vec3 escDir = dir;
    int steps = 0;

    for (int i = 0; i < uMaxSteps; ++i) {
        float r = length(x);
        if (r < R_HORIZON + 0.02) { status = 2; break; }
        float vlen = length(v);
        if (dot(x, v) > 0.0 && r > rEscMin) {
            float b = h / vlen;
            if (uLensing == 0 || 0.75 * b * b * b / (r * r * r * r) < ESC_TOL) {
                escDir = (uLensing != 0) ? farFieldDirection(x, v, h2) : normalize(v);
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
        float dt = ds / vlen;
        vec3 x0 = x, v0 = v;
        rk4Step(x, v, h2, dt);
        steps = i + 1;

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
            float seg = length(x - x0);
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
        if (transmittance < 0.004) { status = 3; break; }
    }
    if (status == 0) status = 2;   // ran out of steps: these rays hug the photon sphere

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

    if (uShadowOverlay != 0) {
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
