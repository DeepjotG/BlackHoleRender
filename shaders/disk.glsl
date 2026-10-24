#ifndef DISK_GLSL
#define DISK_GLSL
#include "common.glsl"
#include "blackbody.glsl"
// Accretion disk: thin-disk surface model and volumetric slab model.

uniform sampler3D uNoise3d;
uniform float uDiskInner;
uniform float uDiskOuter;
uniform float uDiskThickness;     // scale height H / r
uniform float uDiskDensity;
uniform float uDiskOpacity;
uniform float uDiskPeakTemp;
uniform float uDiskBrightness;
uniform float uDiskNoiseScale;
uniform float uDiskNoiseStrength;
uniform float uDiskSpeed;
uniform float uDiskWarmth;

// Film-style warm grade applied to the disk's blackbody colour.
vec3 diskTint(vec3 c) { return c * mix(vec3(1.0), vec3(1.0, 0.80, 0.58), uDiskWarmth); }

float diskSlabHalfHeight(float r) { return 2.5 * uDiskThickness * r; }

// Thin-disk temperature profile T ∝ (r_in/r)^(3/4) (1 - sqrt(r_in/r))^(1/4), normalised to peak 1.
float diskTempProfile(float r) {
    float x = uDiskInner / max(r, uDiskInner);
    float f = pow(x, 0.75) * pow(max(1.0 - sqrt(x), 0.0), 0.25);
    return f / 0.48796;
}

float keplerOmega(float r) { return pow(r, -1.5); }

// Observed/emitted frequency ratio for gas on a circular orbit at r seen by a static
// observer at rcam; bAxis = L_y / E of the photon (sign = sense of disk rotation).
float redshiftFactor(float r, float bAxis, float rcam) {
    float omega = keplerOmega(r);
    float ut = inversesqrt(max(1.0 - 3.0 / r, 0.02));
    float lapse = sqrt(max(1.0 - 2.0 / rcam, 1e-4));
    return 1.0 / (lapse * ut * max(1.0 - omega * bAxis, 0.05));
}

// Tileable noise in disk coordinates, advected with the Keplerian flow. Two phase-shifted
// copies are cross-faded so differential rotation never shears the pattern indefinitely.
float diskNoise(float r, float phi, float yOverH) {
    float omega = keplerOmega(r);
    float t = uTime * uDiskSpeed;
    const float P = 60.0;
    float t1 = mod(t, P);
    float t2 = mod(t + 0.5 * P, P);
    float w2 = abs(2.0 * t1 / P - 1.0);
    float rep = max(1.0, floor(2.0 * uDiskNoiseScale + 0.5));   // integer azimuthal repeats -> seamless
    float lr = log(r) * 3.2 * uDiskNoiseScale;
    float yy = yOverH * 0.35;
    // Disk angular momentum is along +y: gas at (r,0,0) moves toward -z, so phi = atan(z,x) decreases
    // with time. Must match the axis passed to impactParameterAboutAxis in blackhole.frag.
    vec3 c1 = vec3((phi + omega * t1) * (rep / TWO_PI), lr, yy);
    vec3 c2 = vec3((phi + omega * t2) * (rep / TWO_PI), lr + 0.37, yy + 0.61);
    float n1 = texture(uNoise3d, c1).r;
    float n2 = texture(uNoise3d, c2).r;
    return mix(n1, n2, w2);
}

// Optical depth is invariant: the static-frame absorption coefficient equals the fluid-frame one
// scaled by nu'/nu_static = lapseEm / (g * lapseCam). Blended by the Doppler slider.
float opticalDepthScale(float r, float gPhys, float rcam) {
    float lapseEm = sqrt(max(1.0 - 2.0 / r, 1e-4));
    float lapseCam = sqrt(max(1.0 - 2.0 / rcam, 1e-4));
    return mix(1.0, lapseEm / (gPhys * lapseCam), uDopplerStrength);
}

float diskRadialProfile(float r) {
    float inner = smoothstep(uDiskInner, uDiskInner + 0.25, r);
    float outer = 1.0 - smoothstep(uDiskOuter * 0.55, uDiskOuter, r);
    return inner * outer * pow(uDiskInner / max(r, uDiskInner), 1.2);
}

// Returns density; `emit` receives the local surface-brightness multiplier (hot lanes vs dark gaps).
float diskDensity(vec3 p, float r, out float emit) {
    float H = max(uDiskThickness * r, 1e-3);
    float yh = p.y / H;
    float vert = exp(-yh * yh);
    float phi = atan(p.z, p.x);
    float n = diskNoise(r, phi, yh);
    float f = smoothstep(0.32, 0.78, n);
    float pattern = mix(1.0, mix(0.08, 1.4, f), uDiskNoiseStrength);
    emit = mix(1.0, mix(0.22, 1.35, f * f), uDiskNoiseStrength);
    // Turbulent puffing of the outer disk.
    float puff = 1.0 + 0.6 * smoothstep(uDiskOuter * 0.4, uDiskOuter, r) * (n - 0.5);
    return uDiskDensity * diskRadialProfile(r) * vert * pattern * max(puff, 0.0);
}

// Volumetric emission/absorption over one march segment of length ds centred at p.
void volumetricDiskStep(vec3 p, float r, float ds, float bAxis, float rcam, inout vec3 color, inout float transmittance) {
    float emit;
    float dens = diskDensity(p, r, emit);
    if (dens < 1e-4) return;
    float T = uDiskPeakTemp * diskTempProfile(r) * mix(1.0, 0.85 + 0.3 * emit, 0.5);
    float gPhys = redshiftFactor(r, bAxis, rcam);
    float g = mix(1.0, gPhys, uDopplerStrength);
    vec4 bb = blackbody(T * g);
    float inward = pow(uDiskInner / max(r, uDiskInner), 0.6);            // hotter, brighter towards the ISCO
    vec3 source = diskTint(bb.rgb) * (bb.a * uDiskBrightness * emit * inward);   // radiance of an optically thick column
    float absorb = dens * uDiskOpacity * opticalDepthScale(r, gPhys, rcam);
    float a = 1.0 - exp(-absorb * ds);
    color += transmittance * source * a;
    transmittance *= 1.0 - a;
}

// Thin disk: radiance (rgb) and alpha at an equatorial crossing point.
vec4 thinDiskSample(vec3 hit, float bAxis, float rcam) {
    float r = length(hit.xz);
    if (r < uDiskInner || r > uDiskOuter) return vec4(0.0);
    float phi = atan(hit.z, hit.x);
    float n = diskNoise(r, phi, 0.0);
    float f = smoothstep(0.32, 0.78, n);
    float pattern = mix(1.0, mix(0.08, 1.4, f), uDiskNoiseStrength);
    float emit = mix(1.0, mix(0.22, 1.35, f * f), uDiskNoiseStrength);
    float dens = uDiskDensity * diskRadialProfile(r) * pattern;
    float T = uDiskPeakTemp * diskTempProfile(r) * mix(1.0, 0.85 + 0.3 * emit, 0.5);
    float gPhys = redshiftFactor(r, bAxis, rcam);
    float g = mix(1.0, gPhys, uDopplerStrength);
    vec4 bb = blackbody(T * g);
    float alpha = 1.0 - exp(-dens * uDiskOpacity * 0.6 * opticalDepthScale(r, gPhys, rcam));
    vec3 radiance = diskTint(bb.rgb) * (bb.a * uDiskBrightness * emit);
    return vec4(radiance, alpha);
}
#endif
