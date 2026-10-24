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
    float lr = log(r) * 1.4 * uDiskNoiseScale;
    float yy = yOverH * 0.35;
    vec3 c1 = vec3((phi - omega * t1) * (rep / TWO_PI), lr, yy);
    vec3 c2 = vec3((phi - omega * t2) * (rep / TWO_PI), lr + 0.37, yy + 0.61);
    float n1 = texture(uNoise3d, c1).r;
    float n2 = texture(uNoise3d, c2).r;
    return mix(n1, n2, w2);
}

float diskRadialProfile(float r) {
    float inner = smoothstep(uDiskInner, uDiskInner + 0.25, r);
    float outer = 1.0 - smoothstep(uDiskOuter * 0.55, uDiskOuter, r);
    return inner * outer * pow(uDiskInner / max(r, uDiskInner), 1.2);
}

float diskDensity(vec3 p, float r) {
    float H = max(uDiskThickness * r, 1e-3);
    float yh = p.y / H;
    float vert = exp(-yh * yh);
    float phi = atan(p.z, p.x);
    float n = diskNoise(r, phi, yh);
    float filaments = smoothstep(0.25, 0.85, n) * 1.6;
    float pattern = mix(1.0, filaments, uDiskNoiseStrength);
    return uDiskDensity * diskRadialProfile(r) * vert * pattern;
}

// Volumetric emission/absorption over one march segment of length ds centred at p.
void volumetricDiskStep(vec3 p, float r, float ds, float bAxis, float rcam, inout vec3 color, inout float transmittance) {
    float dens = diskDensity(p, r);
    if (dens < 1e-4) return;
    float T = uDiskPeakTemp * diskTempProfile(r);
    float g = mix(1.0, redshiftFactor(r, bAxis, rcam), uDopplerStrength);
    vec4 bb = blackbody(T * g);
    vec3 source = bb.rgb * (bb.a * uDiskBrightness);     // radiance of an optically thick column
    float absorb = dens * uDiskOpacity;
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
    float pattern = mix(1.0, smoothstep(0.2, 0.85, n) * 1.5, uDiskNoiseStrength);
    float dens = uDiskDensity * diskRadialProfile(r) * pattern;
    float T = uDiskPeakTemp * diskTempProfile(r);
    float g = mix(1.0, redshiftFactor(r, bAxis, rcam), uDopplerStrength);
    vec4 bb = blackbody(T * g);
    float alpha = 1.0 - exp(-dens * uDiskOpacity * 0.6);
    vec3 radiance = bb.rgb * (bb.a * uDiskBrightness);
    return vec4(radiance, alpha);
}
#endif
