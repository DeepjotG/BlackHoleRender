#pragma once
// When a piece of star-disruption debris far from the black hole will next pass pericentre. CPU twin of
// the estimate in shaders/gas_count.comp (keep them in sync); tests/tde_director_tests.cpp checks it
// against an integrated orbit.
//
// Far out the orbit is Keplerian to good accuracy. The energy is the exact (conserved) Schwarzschild one,
// so the estimate does not drift as the gas moves: eps = (E^2 - 1) / 2 plays the Newtonian energy's part.
#include <glm/glm.hpp>

#include <cmath>

namespace bhs::tde {

// Coordinate time (M) until the next pericentre of a particle at x with proper velocity v (dx/dtau),
// or a negative number when it is unbound.
inline double timeToNextPericentre(const glm::dvec3& x, const glm::dvec3& v) {
    constexpr double kTwoPi = 6.283185307179586;
    const double r = glm::length(x);
    if (r <= 2.0) return -1.0;
    const double vr = glm::dot(x, v) / r;
    const glm::dvec3 l = glm::cross(x, v);
    const double L2 = glm::dot(l, l);
    const double eps = 0.5 * ((1.0 - 2.0 / r) * (1.0 + L2 / (r * r)) + vr * vr - 1.0);
    if (eps >= 0.0) return -1.0;
    const double a = -0.5 / eps;
    const double e = std::sqrt(std::max(1.0 - L2 / a, 0.0));
    double E = std::acos(std::clamp((1.0 - r / a) / std::max(e, 1e-6), -1.0, 1.0));   // eccentric anomaly
    if (vr < 0.0) E = kTwoPi - E;                                                     // past apocentre
    return (kTwoPi - (E - e * std::sin(E))) * a * std::sqrt(a);
}

}  // namespace bhs::tde
