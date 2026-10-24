#pragma once
// CPU reference for the star-disruption physics of TdeSim. Header-only; needs GLM and Units.h, so
// tests/tde_tests.cpp checks it without OpenGL. shaders/tde_common.glsl mirrors the force law, the
// lapse and the particle sub-cycling defined here: keep the two in sync.
//
// Units: G = c = M_bh = 1 (lengths and times in M), black hole at the origin, +Y = spin axis.
//
// Gravity on massive particles. For a central acceleration f(r) r_hat with conserved
// L = |x cross v|, the orbit u(phi) = 1/r obeys Binet's equation u'' + u = -f(1/u) / (L^2 u^2).
// With
//     a = -x / r^3 - 3 L^2 x / r^5,   i.e.   f(r) = -1/r^2 - 3 L^2 / r^4,
// it becomes u'' + u = 1/L^2 + 3 u^2: the Schwarzschild orbit equation of a massive particle with
// specific angular momentum L. The radial motion matches as well: the conserved quantity
// eps = |v|^2/2 - 1/r - L^2/r^3 equals (E^2 - 1)/2 with E the Schwarzschild specific energy, so
// (dr/dtau)^2 = E^2 - (1 - 2/r)(1 + L^2/r^2). The integration parameter is the particle's proper
// time tau and r is the Schwarzschild radius. Consequences (all exact): circular orbits have
// |v|^2 = 1/(r - 3) and L^2 = r^2/(r - 3), minimal at the ISCO r = 6; the radial epicyclic
// frequency is (r - 6)/(r^3 (r - 3)) per unit proper time, i.e. Omega^2 (1 - 6/r) in coordinate
// time; parabolic orbits (eps = 0) need L^2 >= 16 to turn around, so pericentres inside 4 M
// (L^2 = 16) plunge; the periastron advance is that of Schwarzschild.
//
// The simulation clock is the coordinate time t of a distant observer:
//     dtau/dt = (1 - 2/r) / E,   E^2 = (1 - 2/r)(1 + L^2/r^2) + (dr/dtau)^2.
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "Units.h"

namespace bhs::tde::phys {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGamma = 5.0 / 3.0;          // ideal monatomic gas
constexpr double kCaptureRadius = 4.0;        // parabolic orbits with r_p < 4 M plunge
constexpr double kAccretionRadius = 2.05;     // particles crossing it are removed as accreted
constexpr double kMaxSpeed = 2.0;             // safety clamp on |dx/dtau|
constexpr double kMinLapse = 0.02;            // dtau/dt is clamped here just outside the horizon

// ------------------------------------------------------------------------------------------------
// Black hole gravity, energy, lapse.

inline double angularMomentum2(const glm::dvec3& x, const glm::dvec3& v) {
    const glm::dvec3 l = glm::cross(x, v);
    return glm::dot(l, l);
}

inline glm::dvec3 bhAccel(const glm::dvec3& x, double L2) {
    const double r2 = glm::dot(x, x);
    const double r = std::sqrt(r2);
    return x * (-(1.0 + 3.0 * L2 / r2) / (r2 * r));
}

// Schwarzschild specific energy E of a particle at x with proper velocity v = dx/dtau.
inline double specificEnergy(const glm::dvec3& x, const glm::dvec3& v) {
    const double r = glm::length(x);
    const double vr = glm::dot(x, v) / r;
    const double e2 = (1.0 - 2.0 / r) * (1.0 + angularMomentum2(x, v) / (r * r)) + vr * vr;
    return std::sqrt(std::max(e2, 1e-12));
}

// eps = (E^2 - 1)/2: negative for orbits bound to the black hole.
inline double orbitalEnergy(const glm::dvec3& x, const glm::dvec3& v) {
    const double r = glm::length(x);
    return 0.5 * glm::dot(v, v) - 1.0 / r - angularMomentum2(x, v) / (r * r * r);
}

// dtau/dt, clamped to [kMinLapse, 1].
inline double lapse(const glm::dvec3& x, const glm::dvec3& v) {
    const double r = glm::length(x);
    if (r <= 2.0) return kMinLapse;
    return std::clamp((1.0 - 2.0 / r) / specificEnergy(x, v), kMinLapse, 1.0);
}

// Proper time over which the black hole changes the motion: min(sqrt(r / |a|), 2 r / |v|).
inline double bhTimescale(double r, double L2, double speed) {
    const double tAcc = r * std::sqrt(r) / std::sqrt(1.0 + 3.0 * L2 / (r * r));
    return std::min(tAcc, 2.0 * r / std::max(speed, 1e-30));
}

// Distance of the segment a-b from the origin.
inline double segmentMinRadius(const glm::dvec3& a, const glm::dvec3& b) {
    const glm::dvec3 d = b - a;
    const double dd = glm::dot(d, d);
    const double t = dd > 0.0 ? std::clamp(-glm::dot(a, d) / dd, 0.0, 1.0) : 0.0;
    return glm::length(a + t * d);
}

inline void clampSpeed(glm::dvec3& v) {
    const double s = glm::length(v);
    if (s > kMaxSpeed) v *= kMaxSpeed / s;
}

// ------------------------------------------------------------------------------------------------
// Particle sub-cycling (mirrored by tdeAdvance() in shaders/tde_common.glsl).
//
// Kick-drift-kick leapfrog in proper time; each substep covers eta * bhTimescale of proper time
// (so the step shrinks near pericentre and near the hole), converted to coordinate time with the
// lapse at the start of the substep. At most maxSubsteps substeps are taken: when that is not
// enough the remaining time is split evenly and `capped` is set. In the pure black hole problem
// L is conserved exactly by the scheme (kicks are along x, drifts along v).

struct SubcycleConfig {
    double eta = 0.03;
    int maxSubsteps = 64;
};

enum class Fate { Active, Accreted };

inline Fate advanceParticle(glm::dvec3& x, glm::dvec3& v, double dt, const SubcycleConfig& cfg,
                            const glm::dvec3& aExtra = glm::dvec3(0.0), int* substepsUsed = nullptr,
                            bool* capped = nullptr) {
    double remaining = dt;
    int k = 0;
    if (glm::length(x) < kAccretionRadius) return Fate::Accreted;
    while (remaining > 0.0) {
        const double r = glm::length(x);
        const double L2 = angularMomentum2(x, v);
        const double lap = lapse(x, v);
        const int left = cfg.maxSubsteps - k;
        double step = std::min(cfg.eta * bhTimescale(r, L2, glm::length(v)) / lap, remaining);
        if (left <= 1) {
            step = remaining;
        } else if (step * left < remaining) {
            step = remaining / left;
            if (capped) *capped = true;
        }
        const double dtau = step * lap;
        v += (0.5 * dtau) * (bhAccel(x, L2) + aExtra);
        clampSpeed(v);
        const glm::dvec3 x0 = x;
        x += dtau * v;
        ++k;
        if (segmentMinRadius(x0, x) < kAccretionRadius) {
            if (substepsUsed) *substepsUsed = k;
            return Fate::Accreted;
        }
        v += (0.5 * dtau) * (bhAccel(x, angularMomentum2(x, v)) + aExtra);
        clampSpeed(v);
        remaining -= step;
    }
    if (substepsUsed) *substepsUsed = k;
    return Fate::Active;
}

// ------------------------------------------------------------------------------------------------
// Polytropes: Lane-Emden equation theta'' + (2/xi) theta' + theta^n = 0, theta(0) = 1, theta'(0) = 0.

struct LaneEmden {
    double n = 3.0;
    double xi1 = 0.0;               // first zero of theta (stellar surface)
    double omega = 0.0;             // -xi1^2 theta'(xi1), dimensionless mass
    std::vector<double> theta;      // uniform table on [0, xi1]
    std::vector<double> mass;       // -xi^2 theta'(xi) / omega: enclosed mass fraction

    double step() const { return xi1 / static_cast<double>(theta.size() - 1); }
    double thetaAt(double xi) const { return interp(theta, xi); }
    double massFractionAt(double xi) const { return interp(mass, xi); }
    // Inverse of the enclosed mass fraction (bisection on the monotone table).
    double xiAtMassFraction(double f) const {
        f = std::clamp(f, 0.0, 1.0);
        auto it = std::lower_bound(mass.begin(), mass.end(), f);
        if (it == mass.begin()) return 0.0;
        if (it == mass.end()) return xi1;
        const size_t i = static_cast<size_t>(it - mass.begin());
        const double m0 = mass[i - 1], m1 = mass[i];
        const double t = (m1 > m0) ? (f - m0) / (m1 - m0) : 0.0;
        return (static_cast<double>(i - 1) + t) * step();
    }

private:
    double interp(const std::vector<double>& tab, double xi) const {
        if (xi <= 0.0) return tab.front();
        if (xi >= xi1) return tab.back();
        const double s = xi / step();
        const size_t i = std::min(static_cast<size_t>(s), tab.size() - 2);
        const double t = s - static_cast<double>(i);
        return tab[i] + t * (tab[i + 1] - tab[i]);
    }
};

// RK4 from a series start; the zero is located by linear interpolation of the last step, which is
// accurate to ~1e-9 for the default step.
inline LaneEmden solveLaneEmden(double n, int tableSize = 4097, double h = 1e-4) {
    n = std::clamp(n, 0.5, 4.5);
    LaneEmden le;
    le.n = n;
    auto f = [n](double xi, double th, double dth) {
        return -std::pow(std::max(th, 0.0), n) - 2.0 * dth / xi;
    };
    double xi = 1e-3;
    double th = 1.0 - xi * xi / 6.0 + n * std::pow(xi, 4) / 120.0;
    double dth = -xi / 3.0 + n * std::pow(xi, 3) / 30.0;
    std::vector<double> xs{0.0, xi}, ths{1.0, th}, dths{0.0, dth};
    for (int it = 0; it < 50000000; ++it) {
        const double k1t = dth, k1d = f(xi, th, dth);
        const double k2t = dth + 0.5 * h * k1d, k2d = f(xi + 0.5 * h, th + 0.5 * h * k1t, dth + 0.5 * h * k1d);
        const double k3t = dth + 0.5 * h * k2d, k3d = f(xi + 0.5 * h, th + 0.5 * h * k2t, dth + 0.5 * h * k2d);
        const double k4t = dth + h * k3d, k4d = f(xi + h, th + h * k3t, dth + h * k3d);
        const double thN = th + h / 6.0 * (k1t + 2.0 * k2t + 2.0 * k3t + k4t);
        const double dthN = dth + h / 6.0 * (k1d + 2.0 * k2d + 2.0 * k3d + k4d);
        if (thN <= 0.0) {
            const double t = th / (th - thN);
            le.xi1 = xi + t * h;
            const double dth1 = dth + t * (dthN - dth);
            le.omega = -le.xi1 * le.xi1 * dth1;
            xs.push_back(le.xi1);
            ths.push_back(0.0);
            dths.push_back(dth1);
            break;
        }
        xi += h;
        th = thN;
        dth = dthN;
        xs.push_back(xi);
        ths.push_back(th);
        dths.push_back(dth);
    }
    // Resample onto a uniform table.
    le.theta.resize(static_cast<size_t>(tableSize));
    le.mass.resize(static_cast<size_t>(tableSize));
    size_t j = 0;
    for (int i = 0; i < tableSize; ++i) {
        const double x = le.xi1 * i / (tableSize - 1);
        while (j + 2 < xs.size() && xs[j + 1] < x) ++j;
        const double t = std::clamp((x - xs[j]) / (xs[j + 1] - xs[j]), 0.0, 1.0);
        const double thx = ths[j] + t * (ths[j + 1] - ths[j]);
        const double dthx = dths[j] + t * (dths[j + 1] - dths[j]);
        le.theta[static_cast<size_t>(i)] = std::max(thx, 0.0);
        le.mass[static_cast<size_t>(i)] = std::clamp(-x * x * dthx / le.omega, 0.0, 1.0);
    }
    le.mass.front() = 0.0;
    le.mass.back() = 1.0;
    for (size_t i = 1; i < le.mass.size(); ++i) le.mass[i] = std::max(le.mass[i], le.mass[i - 1]);
    return le;
}

// Dimensional polytrope of mass q (black hole units) and radius R (M), with G = 1. Densities are in
// "mass fraction" units rho' = rho / q (the GPU grids store fractions of the star's mass), so that
// the pressure acceleration -grad(P)/rho is the same in both.
struct PolytropeModel {
    double q = 0.0, R = 0.0;
    double rhoC = 0.0;        // central rho' (1/M^3)
    double pOverRhoC = 0.0;   // P_c / rho_c (c^2)
    double Kp = 0.0;          // P/rho = Kp * rho'^(1/n)
    double meanRho = 0.0;     // 1 / (4/3 pi R^3)
    // Specific internal energy u = P / ((gamma - 1) rho) at dimensionless radius xi.
    double u(const LaneEmden& le, double xi) const { return pOverRhoC * le.thetaAt(xi) / (kGamma - 1.0); }
};

inline PolytropeModel makePolytrope(const LaneEmden& le, double q, double R) {
    PolytropeModel m;
    m.q = q;
    m.R = R;
    const double alpha = R / le.xi1;
    m.rhoC = 1.0 / (4.0 * kPi * alpha * alpha * alpha * le.omega);
    m.pOverRhoC = q * le.xi1 / (R * le.omega * (le.n + 1.0));
    m.Kp = m.pOverRhoC / std::pow(m.rhoC, 1.0 / le.n);
    m.meanRho = 1.0 / (4.0 / 3.0 * kPi * R * R * R);
    return m;
}

// Equal-mass particle positions (units of the stellar radius) sampling the polytrope: stratified
// samples (one uniformly jittered point per cell of a randomly rotated cubic lattice) inside the unit
// ball, sorted by radius, then stretched radially so that the i-th point sits at the radius enclosing
// a mass fraction (i + 0.5) / N. This keeps the density field far quieter than random sampling,
// without lattice planes that would show as stripes once the star is stretched into a stream.
inline std::vector<glm::dvec3> samplePolytrope(const LaneEmden& le, int count, uint32_t seed) {
    count = std::max(count, 1);
    std::mt19937 rng(seed * 2654435761u + 97u);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    // Random rotation (from a random unit quaternion).
    glm::dvec4 qv;
    do {
        qv = glm::dvec4(U(rng), U(rng), U(rng), U(rng));
    } while (glm::dot(qv, qv) < 1e-3 || glm::dot(qv, qv) > 1.0);
    qv /= std::sqrt(glm::dot(qv, qv));
    const double a = qv.w, b = qv.x, c = qv.y, d = qv.z;
    const glm::dmat3 rot(a * a + b * b - c * c - d * d, 2 * (b * c + a * d), 2 * (b * d - a * c),
                         2 * (b * c - a * d), a * a - b * b + c * c - d * d, 2 * (c * d + a * b),
                         2 * (b * d + a * c), 2 * (c * d - a * b), a * a - b * b - c * c + d * d);
    std::vector<glm::dvec3> pts;
    double spacing = std::cbrt(4.0 * kPi / (3.0 * count));
    for (int attempt = 0; attempt < 8; ++attempt) {
        pts.clear();
        const int half = static_cast<int>(std::ceil(1.0 / spacing)) + 1;
        for (int i = -half; i <= half; ++i)
            for (int j = -half; j <= half; ++j)
                for (int k = -half; k <= half; ++k) {
                    glm::dvec3 p = (glm::dvec3(i, j, k) + 0.5 + 0.5 * glm::dvec3(U(rng), U(rng), U(rng))) * spacing;
                    p = rot * p;
                    if (glm::dot(p, p) < 1.0) pts.push_back(p);
                }
        if (static_cast<int>(pts.size()) >= count) break;
        spacing *= 0.97;
    }
    std::sort(pts.begin(), pts.end(), [](const glm::dvec3& x, const glm::dvec3& y) { return glm::dot(x, x) < glm::dot(y, y); });
    pts.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        glm::dvec3& p = pts[static_cast<size_t>(i)];
        const double len = glm::length(p);
        const glm::dvec3 dir = len > 1e-12 ? p / len : glm::dvec3(0.0, 1.0, 0.0);
        const double f = (static_cast<double>(i) + 0.5) / static_cast<double>(count);
        p = dir * (le.xiAtMassFraction(f) / le.xi1);
    }
    return pts;
}

// ------------------------------------------------------------------------------------------------
// The event: tidal radius, pericentre, parabolic orbit.

struct EventInputs {
    double starMassSolar = 1.0;
    double starRadiusSolar = 1.0;
    double bhMassSolar = 1e6;
    double beta = 1.0;
    double inclinationDeg = 20.0;
    double longitudeDeg = 0.0;
    bool retrograde = false;
    double startDistanceRt = 3.0;
};

struct EventSetup {
    double q = 0.0;              // star mass / black hole mass
    double R = 0.0;              // star radius (M)
    double rt = 0.0;             // tidal radius R q^(-1/3)
    double rp = 0.0;             // pericentre r_t / beta
    double rStart = 0.0;
    double rRelease = 0.0;       // particles are released inside this distance
    bool swallowed = false;      // r_p < kCaptureRadius
    double L2 = 0.0;             // specific angular momentum squared of the orbit
    glm::dvec3 nHat{0.0, 1.0, 0.0};   // orbit normal (direction of the angular momentum)
    glm::dvec3 pHat{1.0, 0.0, 0.0};   // direction of the pericentre
    glm::dvec3 x0{0.0}, v0{0.0};      // initial centre-of-mass position and proper velocity
};

// Specific angular momentum squared of the parabolic (E = 1) orbit with pericentre rp. For rp >= 4
// it is the exact Schwarzschild value 2 rp^2 / (rp - 2) (16 at rp = 4); smaller "pericentres" have no
// turning point and plunge, and continue as 4 rp (< 16).
inline double parabolicL2(double rp) { return rp >= kCaptureRadius ? 2.0 * rp * rp / (rp - 2.0) : 4.0 * rp; }

inline double finiteOr(double x, double fallback) { return std::isfinite(x) ? x : fallback; }

inline EventSetup setupEvent(EventInputs in) {
    in.starMassSolar = std::clamp(finiteOr(in.starMassSolar, 1.0), 1e-3, 1e3);
    in.starRadiusSolar = std::clamp(finiteOr(in.starRadiusSolar, 1.0), 1e-4, 1e5);
    in.bhMassSolar = std::clamp(finiteOr(in.bhMassSolar, 1e6), 1.0, 1e13);
    in.beta = std::clamp(finiteOr(in.beta, 1.0), 0.05, 100.0);
    in.startDistanceRt = std::clamp(finiteOr(in.startDistanceRt, 3.0), 0.5, 1e4);
    const double incl = glm::radians(finiteOr(in.inclinationDeg, 0.0));
    const double lon = glm::radians(finiteOr(in.longitudeDeg, 0.0));

    EventSetup s;
    s.q = in.starMassSolar / in.bhMassSolar;
    s.R = in.starRadiusSolar * units::kSolarRadius / units::metersPerM(in.bhMassSolar);
    s.rt = s.R * std::cbrt(1.0 / s.q);
    s.rp = s.rt / in.beta;
    s.swallowed = s.rp < kCaptureRadius;
    s.rStart = std::max(in.startDistanceRt * s.rt, 1.5 * s.rp);
    if (s.swallowed) s.rStart = std::max(s.rStart, 12.0);   // keep the plunge of an intact star visible
    s.rRelease = std::max({2.0 * s.rt, 1.05 * s.rp, s.rt + 2.0 * s.R});
    s.L2 = parabolicL2(s.rp);

    // Pericentre direction: +X rotated by the longitude about +Y (right-hand rule, i.e. in the sense of
    // the disk's rotation). The orbit plane contains it and is tilted by the inclination about it:
    // n = Y cos(i) + (p x Y) sin(i); a retrograde orbit reverses n.
    s.pHat = glm::dvec3(std::cos(lon), 0.0, -std::sin(lon));
    s.nHat = glm::dvec3(std::sin(lon) * std::sin(incl), std::cos(incl), std::cos(lon) * std::sin(incl));
    if (in.retrograde) s.nHat = -s.nHat;
    const glm::dvec3 tHat = glm::cross(s.nHat, s.pHat);   // direction of motion at pericentre

    if (!s.swallowed) {
        // Integrate outwards from pericentre, then mirror the state onto the inbound branch.
        glm::dvec3 x = s.rp * s.pHat;
        glm::dvec3 v = (std::sqrt(s.L2) / s.rp) * tHat;
        SubcycleConfig cfg{2e-4, 1 << 30};
        for (int it = 0; it < 10000000 && glm::length(x) < s.rStart; ++it) {
            const double r = glm::length(x);
            const double dtau = cfg.eta * bhTimescale(r, s.L2, glm::length(v));
            const double r1 = glm::length(x + dtau * v);
            double h = dtau;
            if (r1 > s.rStart && r1 > r) h = dtau * (s.rStart - r) / (r1 - r);   // land on rStart
            v += (0.5 * h) * bhAccel(x, angularMomentum2(x, v));
            x += h * v;
            v += (0.5 * h) * bhAccel(x, angularMomentum2(x, v));
            if (h < dtau) break;
        }
        const glm::dvec3 xin = glm::dot(x, s.pHat) * s.pHat - glm::dot(x, tHat) * tHat;
        const glm::dvec3 vin = -glm::dot(v, s.pHat) * s.pHat + glm::dot(v, tHat) * tHat;
        s.x0 = xin;
        s.v0 = vin;
    } else {
        // No turning point: place the star on the Newtonian parabola's angle for orientation and give
        // it the exact E = 1 radial and tangential velocity.
        const double r = s.rStart;
        const double cosPhi = std::clamp(2.0 * s.rp / r - 1.0, -1.0, 1.0);
        const double phi = std::acos(cosPhi);
        const glm::dvec3 rhat = std::cos(phi) * s.pHat - std::sin(phi) * tHat;
        const glm::dvec3 phat = glm::cross(s.nHat, rhat);
        const double vr2 = 1.0 - (1.0 - 2.0 / r) * (1.0 + s.L2 / (r * r));
        s.x0 = r * rhat;
        s.v0 = -std::sqrt(std::max(vr2, 0.0)) * rhat + (std::sqrt(s.L2) / r) * phat;
    }
    return s;
}

}  // namespace bhs::tde::phys
