#pragma once
// CPU mirror of shaders/geodesic.glsl. Schwarzschild null geodesics in
// Cartesian-like coordinates with G = c = M = 1 (horizon r = 2).
//
// The spatial path of a photon obeys   x'' = -3 h^2 x / r^5,   h = |x × v| (conserved),
// which is exact for the orbit shape (Binet: u'' + u = 3 u^2). The parameter is a
// Newtonian-like time, not the affine parameter; only the path matters here.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace bhs::physics {

constexpr double kHorizon = 2.0;
constexpr double kPhotonSphere = 3.0;
constexpr double kIsco = 6.0;

inline double criticalImpactParameter() { return 3.0 * std::sqrt(3.0); }

inline glm::dvec3 accel(const glm::dvec3& x, double h2) {
    const double r2 = glm::dot(x, x);
    const double r = std::sqrt(r2);
    return x * (-3.0 * h2 / (r2 * r2 * r));
}

inline void rk4Step(glm::dvec3& x, glm::dvec3& v, double h2, double dt) {
    const glm::dvec3 k1v = accel(x, h2), k1x = v;
    const glm::dvec3 k2v = accel(x + 0.5 * dt * k1x, h2), k2x = v + 0.5 * dt * k1v;
    const glm::dvec3 k3v = accel(x + 0.5 * dt * k2x, h2), k3x = v + 0.5 * dt * k2v;
    const glm::dvec3 k4v = accel(x + dt * k3x, h2), k4x = v + dt * k3v;
    x += (dt / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
    v += (dt / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
}

// Coordinate velocity direction for a photon leaving a static observer at camPos
// with local (orthonormal-frame) direction dir. The radial component is scaled by
// sqrt(1 - 2/r); with h = |x × v| this reproduces the exact impact parameter
// b = r sin(alpha) / sqrt(1 - 2/r).
inline glm::dvec3 initialVelocity(const glm::dvec3& camPos, const glm::dvec3& dir) {
    const double r = glm::length(camPos);
    const glm::dvec3 rhat = camPos / r;
    const double nr = glm::dot(dir, rhat);
    const glm::dvec3 nperp = dir - nr * rhat;
    const double f = std::sqrt(std::max(1.0 - 2.0 / r, 0.0));
    return glm::normalize(nr * f * rhat + nperp);
}

// Impact parameter of the ray (b = h / |v_inf|), from the initial state.
inline double impactParameter(const glm::dvec3& camPos, const glm::dvec3& dir) {
    const double r = glm::length(camPos);
    const double sinA = glm::length(glm::cross(camPos / r, dir));
    return r * sinA / std::sqrt(std::max(1.0 - 2.0 / r, 1e-12));
}

// Photon angular momentum about `axis` divided by its energy (L_axis / E), for the
// photon that arrives at the camera travelling along -dir (we trace backwards).
inline double impactParameterAboutAxis(const glm::dvec3& camPos, const glm::dvec3& dir, const glm::dvec3& axis) {
    const double r = glm::length(camPos);
    return -glm::dot(glm::cross(camPos, dir), axis) / std::sqrt(std::max(1.0 - 2.0 / r, 1e-12));
}

// First-order correction for the deflection remaining between (x, v) and infinity,
// assuming the rest of the path is nearly straight. Valid for outgoing rays.
inline glm::dvec3 farFieldDirection(const glm::dvec3& x, const glm::dvec3& v, double h2) {
    const double vlen = glm::length(v);
    const glm::dvec3 vh = v / vlen;
    const double r = glm::length(x);
    const double p = glm::dot(x, vh);
    const glm::dvec3 bvec = x - p * vh;
    const double s = std::clamp(p / r, 0.0, 1.0);
    const double r3 = r * r * r;
    const glm::dvec3 dv = -(h2 / vlen) * (vh / r3 + bvec * ((2.0 + s) / (r3 * r * (1.0 + s) * (1.0 + s))));
    return glm::normalize(v + dv);
}

// Angular radius of the black hole shadow for a static observer at radius r.
inline double shadowAngularRadius(double r) {
    const double s = std::min(1.0, criticalImpactParameter() / r * std::sqrt(std::max(1.0 - 2.0 / r, 0.0)));
    const double a = std::asin(s);
    return (r >= kPhotonSphere) ? a : (M_PI - a);
}

// Keplerian angular velocity of circular orbits (M = 1).
inline double keplerOmega(double r) { return std::pow(r, -1.5); }

// Ratio observed/emitted frequency for light from gas on a circular equatorial
// orbit at radius rEmit, seen by a static observer at rCam. bAxis = L_axis / E of
// the photon (positive when it shares the disk's sense of rotation).
inline double redshiftFactor(double rEmit, double bAxis, double rCam) {
    const double omega = keplerOmega(rEmit);
    const double ut = 1.0 / std::sqrt(std::max(1.0 - 3.0 / rEmit, 1e-6));
    const double lapseCam = std::sqrt(std::max(1.0 - 2.0 / rCam, 1e-6));
    return 1.0 / (lapseCam * ut * (1.0 - omega * bAxis));
}

struct TraceResult {
    enum Status { Escaped, Captured, MaxSteps } status = MaxSteps;
    glm::dvec3 dir{0.0};    // asymptotic direction if escaped
    int steps = 0;
    double minR = 1e30;
    int equatorCrossings = 0;
};

// Traces a ray from camPos in local direction dir with the same control logic as
// the shader (step ∝ r, tolerance-based escape with far-field correction).
inline TraceResult trace(const glm::dvec3& camPos, const glm::dvec3& dir, int maxSteps = 200000,
                         double stepScale = 0.005, double escTol = 1e-4, double rEscMin = 30.0) {
    TraceResult res;
    glm::dvec3 x = camPos;
    glm::dvec3 v = initialVelocity(camPos, dir);
    const glm::dvec3 hv = glm::cross(x, v);
    const double h2 = glm::dot(hv, hv);
    const double h = std::sqrt(h2);
    double prevY = x.y;
    for (int i = 0; i < maxSteps; ++i) {
        const double r = glm::length(x);
        res.minR = std::min(res.minR, r);
        if (r < kHorizon + 0.01) {
            res.status = TraceResult::Captured;
            res.steps = i;
            return res;
        }
        const double vlen = glm::length(v);
        const double b = h / vlen;
        if (glm::dot(x, v) > 0.0 && r > rEscMin && 0.75 * b * b * b / (r * r * r * r) < escTol) {
            res.status = TraceResult::Escaped;
            res.dir = farFieldDirection(x, v, h2);
            res.steps = i;
            return res;
        }
        const double ds = std::max(stepScale * r, 1e-4);
        rk4Step(x, v, h2, ds / vlen);
        if ((x.y > 0.0) != (prevY > 0.0)) ++res.equatorCrossings;
        prevY = x.y;
        res.steps = i + 1;
    }
    res.status = TraceResult::MaxSteps;
    return res;
}

}  // namespace bhs::physics
