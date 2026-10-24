#ifndef GEODESIC_GLSL
#define GEODESIC_GLSL
#include "common.glsl"
// Schwarzschild null geodesics in Cartesian-like coordinates (G = c = M = 1).
// The spatial path obeys x'' = -3 h^2 x / r^5 with h = |x × v| conserved; this is exact
// for the orbit shape (Binet equation u'' + u = 3u^2). See src/physics/Geodesic.h.

const float ESC_TOL = 0.01;   // remaining first-order deflection (rad) allowed when leaving the march

vec3 geoAccel(vec3 x, float h2) {
    float r2 = dot(x, x);
    float r = sqrt(r2);
    return x * (-3.0 * h2 / (r2 * r2 * r));
}

void rk4Step(inout vec3 x, inout vec3 v, float h2, float dt) {
    vec3 k1v = geoAccel(x, h2);                   vec3 k1x = v;
    vec3 k2v = geoAccel(x + 0.5 * dt * k1x, h2);  vec3 k2x = v + 0.5 * dt * k1v;
    vec3 k3v = geoAccel(x + 0.5 * dt * k2x, h2);  vec3 k3x = v + 0.5 * dt * k2v;
    vec3 k4v = geoAccel(x + dt * k3x, h2);        vec3 k4x = v + dt * k3v;
    x += (dt / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
    v += (dt / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
}

// Coordinate velocity for a photon leaving a static observer at camPos with local
// direction dir: the radial component is contracted by sqrt(1 - 2/r).
vec3 initialVelocity(vec3 camPos, vec3 dir) {
    float r = length(camPos);
    vec3 rhat = camPos / r;
    float nr = dot(dir, rhat);
    vec3 nperp = dir - nr * rhat;
    float f = sqrt(max(1.0 - 2.0 / r, 0.0));
    return normalize(nr * f * rhat + nperp);
}

// L_axis / E for the photon arriving along -dir (we trace backwards from the camera).
float impactParameterAboutAxis(vec3 camPos, vec3 dir, vec3 axis) {
    float r = length(camPos);
    return -dot(cross(camPos, dir), axis) * inversesqrt(max(1.0 - 2.0 / r, 1e-6));
}

// First-order correction for the deflection still to come on an outgoing ray.
vec3 farFieldDirection(vec3 x, vec3 v, float h2) {
    float vlen = length(v);
    vec3 vh = v / vlen;
    float r = length(x);
    float p = dot(x, vh);
    vec3 bvec = x - p * vh;
    float s = clamp(p / r, 0.0, 1.0);
    float r3 = r * r * r;
    vec3 dv = -(h2 / vlen) * (vh / r3 + bvec * ((2.0 + s) / (r3 * r * (1.0 + s) * (1.0 + s))));
    return normalize(v + dv);
}

// Angular radius of the shadow seen by a static observer at radius r.
float shadowAngularRadius(float r) {
    float s = min(1.0, B_CRIT / r * sqrt(max(1.0 - 2.0 / r, 0.0)));
    float a = asin(s);
    return (r >= R_PHOTON) ? a : (PI - a);
}
#endif
