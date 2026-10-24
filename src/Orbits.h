#pragma once
// Keplerian orbits from JPL-style mean elements ("Keplerian Elements for Approximate Positions of
// the Major Planets", E. M. Standish): heliocentric ecliptic J2000 positions in AU.
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Units.h"
#include "universe/Universe.h"

namespace bhs::orbits {

constexpr double kPi = 3.14159265358979323846;
constexpr double kJ2000 = 2451545.0;   // Julian date of the J2000 epoch

// Solves Kepler's equation E - e sin E = M (radians) by Newton iteration.
inline double eccentricAnomaly(double M, double e) {
    M = std::remainder(M, 2.0 * kPi);
    double E = e < 0.8 ? M : (M < 0.0 ? -kPi : kPi);
    for (int i = 0; i < 30; ++i) {
        const double dE = (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
        E -= dE;
        if (std::abs(dE) < 1e-14) break;
    }
    return E;
}

// Heliocentric ecliptic J2000 position (AU) at T Julian centuries after J2000.
// The secular rates are fits valid for a few thousand years: beyond +-30 centuries the orbit's shape
// and orientation stay at their limit while the planet keeps moving along it.
inline double secularT(double T) { return std::clamp(T, -30.0, 30.0); }

inline glm::dvec3 position(const universe::OrbitElements& el, double T) {
    const double d2r = kPi / 180.0;
    const double Ts = secularT(T);
    const double a = std::max(el.aAU + el.aRate * Ts, 1e-6);
    const double e = std::clamp(el.e + el.eRate * Ts, 0.0, 0.99);
    const double I = (el.iDeg + el.iRate * Ts) * d2r, L = (el.LDeg + std::fmod(el.LRate * T, 360.0)) * d2r;
    const double varpi = (el.varpiDeg + el.varpiRate * Ts) * d2r, Om = (el.OmegaDeg + el.OmegaRate * Ts) * d2r;
    const double w = varpi - Om;
    const double E = eccentricAnomaly(L - varpi, e);
    const double xp = a * (std::cos(E) - e), yp = a * std::sqrt(1.0 - e * e) * std::sin(E);
    const double cw = std::cos(w), sw = std::sin(w), cO = std::cos(Om), sO = std::sin(Om), cI = std::cos(I), sI = std::sin(I);
    return {(cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
            (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
            (sw * sI) * xp + (cw * sI) * yp};
}

// Unit normal of the orbit plane (ecliptic J2000).
inline glm::dvec3 pole(const universe::OrbitElements& el, double T) {
    const double d2r = kPi / 180.0;
    const double Ts = secularT(T);
    const double I = (el.iDeg + el.iRate * Ts) * d2r, Om = (el.OmegaDeg + el.OmegaRate * Ts) * d2r;
    return {std::sin(I) * std::sin(Om), -std::sin(I) * std::cos(Om), std::cos(I)};
}

// Orbital period (years) from the mean-longitude rate.
inline double periodYears(const universe::OrbitElements& el) { return el.LRate != 0.0 ? 36000.0 / std::abs(el.LRate) : 0.0; }

inline double julianDateNow() {
    using namespace std::chrono;
    const double unix = duration<double>(system_clock::now().time_since_epoch()).count();
    return 2440587.5 + unix / units::kDay;
}

// Equatorial (ICRS) unit vector -> ecliptic J2000.
inline glm::dvec3 equatorialToEcliptic(const glm::dvec3& eq) {
    const double eps = 23.43928 * kPi / 180.0;
    return {eq.x, std::cos(eps) * eq.y + std::sin(eps) * eq.z, -std::sin(eps) * eq.y + std::cos(eps) * eq.z};
}

}  // namespace bhs::orbits
