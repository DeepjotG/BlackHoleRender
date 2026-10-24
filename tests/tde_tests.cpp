// CPU checks for the star-disruption physics (src/tde/TdePhysics.h, mirrored by the tde shaders).
// Build: g++ -std=c++20 -O2 -I src -I <glm> tests/tde_tests.cpp -o tde_tests
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tde/TdePhysics.h"

using namespace bhs::tde::phys;
using glm::dvec3;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        char msg_[256];                                                        \
        std::snprintf(msg_, sizeof msg_, __VA_ARGS__);                         \
        if (cond) {                                                            \
            std::printf("  ok    %s\n", msg_);                                 \
        } else {                                                               \
            ++g_failures;                                                      \
            std::printf("  FAIL  %s   (%s:%d)\n", msg_, __FILE__, __LINE__);   \
        }                                                                      \
    } while (0)

static double azimuth(const dvec3& x) { return std::atan2(-x.z, x.x); }   // increases along prograde motion

// Integrates in coordinate-time chunks until `stop` returns true or tMax is reached.
template <class Stop>
static Fate integrate(dvec3& x, dvec3& v, double tMax, double chunk, const SubcycleConfig& cfg, Stop stop) {
    double t = 0.0;
    while (t < tMax) {
        const Fate f = advanceParticle(x, v, chunk, cfg);
        t += chunk;
        if (f == Fate::Accreted) return f;
        if (stop(t, x, v)) break;
    }
    return Fate::Active;
}

static void testCircularOrbits() {
    std::printf("circular orbits: |v|^2 = 1/(r-3), L^2 = r^2/(r-3)\n");
    // Leapfrog started from the exact circular state rides a slightly eccentric "shadow" orbit whose
    // amplitude scales as eta^2: ~0.3% at the GPU's eta = 0.03, ~0.03% at eta = 0.01.
    for (double eta : {0.03, 0.01}) {
        const SubcycleConfig cfg{eta, 1 << 20};
        const double tol = eta > 0.02 ? 5e-3 : 6e-4;
        for (double r0 : {7.0, 10.0, 40.0}) {
            dvec3 x(r0, 0, 0), v(0, 0, -1.0 / std::sqrt(r0 - 3.0));
            double rmin = r0, rmax = r0;
            const double period = 2.0 * kPi * std::pow(r0, 1.5);   // coordinate-time period
            integrate(x, v, 5.0 * period, period / 50.0, cfg, [&](double, const dvec3& p, const dvec3&) {
                rmin = std::min(rmin, glm::length(p));
                rmax = std::max(rmax, glm::length(p));
                return false;
            });
            CHECK(rmax - r0 < tol * r0 && r0 - rmin < tol * r0,
                  "eta=%.2f: circular orbit at r=%.0f stays circular over 5 orbits (r in [%.5f, %.5f])", eta, r0, rmin, rmax);
        }
    }
    // L^2(r) = r^2/(r-3) has its minimum at the ISCO.
    auto L2c = [](double r) { return r * r / (r - 3.0); };
    CHECK(L2c(5.99) > L2c(6.0) && L2c(6.01) > L2c(6.0), "L^2 of circular orbits is minimal at r = 6 (ISCO)");
}

static void testIsco() {
    std::printf("ISCO at 6 M\n");
    const SubcycleConfig cfg{0.03, 4096};
    // Slightly perturbed circular orbits: stable outside 6 M, plunge inside. The kick keeps L^2 above
    // the ISCO value 12 (a 0.5% cut at r = 6.6 would not, and that orbit correctly plunges).
    for (double r0 : {6.6, 7.5}) {
        dvec3 x(r0, 0, 0), v(0, 0, -0.9995 / std::sqrt(r0 - 3.0));
        double rmin = r0;
        const Fate f = integrate(x, v, 20.0 * 2.0 * kPi * std::pow(r0, 1.5), 5.0, cfg, [&](double, const dvec3& p, const dvec3&) {
            rmin = std::min(rmin, glm::length(p));
            return false;
        });
        CHECK(f == Fate::Active && rmin > 6.0, "perturbed circular orbit at r=%.1f is stable for 20 orbits (rmin %.3f)", r0, rmin);
    }
    for (double r0 : {5.5, 5.9}) {
        dvec3 x(r0, 0, 0), v(0, 0, -0.999 / std::sqrt(r0 - 3.0));
        const Fate f = integrate(x, v, 40.0 * 2.0 * kPi * std::pow(r0, 1.5), 5.0, cfg, [](double, const dvec3&, const dvec3&) { return false; });
        CHECK(f == Fate::Accreted, "perturbed circular orbit at r=%.1f plunges into the hole", r0);
    }
}

// Coordinate-time radial period and periastron advance of a near-circular orbit at r = 10:
// kappa^2 = Omega^2 (1 - 6/r), advance per radial period 2 pi (1/sqrt(1 - 6/r) - 1).
static void testEpicycles() {
    std::printf("radial epicycles at r = 10\n");
    const double r0 = 10.0;
    const SubcycleConfig cfg{0.03, 4096};
    // Starts at apocentre with a 1e-4 velocity cut: small enough that the finite-amplitude correction
    // to the epicyclic frequencies is negligible (a 1e-3 cut shifts the advance by 0.7%).
    dvec3 x(r0, 0, 0), v(0, 0, -0.9999 / std::sqrt(r0 - 3.0));
    std::vector<double> apoT, apoPhi;
    double t = 0.0, prevVr = 0.0, unwrapped = 0.0, prevPhi = azimuth(x);
    const double chunk = 0.5;
    while (apoT.size() < 3 && t < 5e4) {
        advanceParticle(x, v, chunk, cfg);
        t += chunk;
        double phi = azimuth(x);
        double dphi = phi - prevPhi;
        if (dphi < -kPi) dphi += 2.0 * kPi;
        if (dphi > kPi) dphi -= 2.0 * kPi;
        unwrapped += dphi;
        prevPhi = phi;
        const double vr = glm::dot(x, v);
        if (prevVr > 0.0 && vr <= 0.0) {   // apocentre between the two samples
            const double f = prevVr / (prevVr - vr);
            apoT.push_back(t - chunk + f * chunk);
            apoPhi.push_back(unwrapped - dphi + f * dphi);
        }
        prevVr = vr;
    }
    const double Tr = apoT.size() >= 3 ? apoT[2] - apoT[1] : 0.0;
    const double adv = apoT.size() >= 3 ? apoPhi[2] - apoPhi[1] - 2.0 * kPi : 0.0;
    const double TrExp = 2.0 * kPi / std::sqrt(std::pow(r0, -3.0) * (1.0 - 6.0 / r0));
    const double advExp = 2.0 * kPi * (1.0 / std::sqrt(1.0 - 6.0 / r0) - 1.0);
    CHECK(std::abs(Tr - TrExp) < 0.005 * TrExp, "radial period %.2f vs Schwarzschild %.2f", Tr, TrExp);
    CHECK(std::abs(adv - advExp) < 0.005 * advExp, "periastron advance %.4f rad vs %.4f", adv, advExp);
}

// Exact Schwarzschild periastron advance between r_p and r_a by quadrature.
static double exactAdvance(double rp, double ra) {
    const double L2 = 2.0 * (1.0 / rp - 1.0 / ra) / ((1.0 - 2.0 / rp) / (rp * rp) - (1.0 - 2.0 / ra) / (ra * ra));
    const double E2 = (1.0 - 2.0 / rp) * (1.0 + L2 / (rp * rp));
    const double L = std::sqrt(L2);
    const int n = 200000;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {   // midpoint rule in chi; r = (ra+rp)/2 - (ra-rp)/2 cos(chi)
        const double chi = kPi * (i + 0.5) / n;
        const double r = 0.5 * (ra + rp) - 0.5 * (ra - rp) * std::cos(chi);
        const double drdchi = 0.5 * (ra - rp) * std::sin(chi);
        const double rad = E2 - (1.0 - 2.0 / r) * (1.0 + L2 / (r * r));
        sum += L / (r * r) * drdchi / std::sqrt(std::max(rad, 1e-300));
    }
    return 2.0 * sum * (kPi / n) - 2.0 * kPi;
}

static void testEccentricPrecession() {
    std::printf("eccentric orbit periastron advance\n");
    for (auto [rp, ra] : {std::pair{20.0, 60.0}, std::pair{12.0, 200.0}}) {
        const double L2 = 2.0 * (1.0 / rp - 1.0 / ra) / ((1.0 - 2.0 / rp) / (rp * rp) - (1.0 - 2.0 / ra) / (ra * ra));
        dvec3 x(ra, 0, 0), v(0, 0, -std::sqrt(L2) / ra);
        const SubcycleConfig cfg{0.03, 4096};
        double unwrapped = 0.0, prevPhi = azimuth(x), prevVr = 0.0;
        std::vector<double> apo;
        const double chunk = 2.0;
        for (int i = 0; i < 2000000 && apo.size() < 2; ++i) {
            advanceParticle(x, v, chunk, cfg);
            double dphi = azimuth(x) - prevPhi;
            if (dphi < -kPi) dphi += 2.0 * kPi;
            if (dphi > kPi) dphi -= 2.0 * kPi;
            unwrapped += dphi;
            prevPhi = azimuth(x);
            const double vr = glm::dot(x, v);
            if (prevVr > 0.0 && vr <= 0.0) {
                const double f = prevVr / (prevVr - vr);
                apo.push_back(unwrapped - dphi + f * dphi);
            }
            prevVr = vr;
        }
        const double got = apo.size() >= 1 ? apo[0] - 2.0 * kPi : 0.0;
        const double expect = exactAdvance(rp, ra);
        CHECK(std::abs(got - expect) < 0.01 * expect, "rp=%.0f ra=%.0f: advance %.5f rad vs exact %.5f", rp, ra, got, expect);
    }
}

static EventInputs inputs(double mStar, double rStar, double mBh, double beta, double incl, double lon, bool retro) {
    EventInputs in;
    in.starMassSolar = mStar;
    in.starRadiusSolar = rStar;
    in.bhMassSolar = mBh;
    in.beta = beta;
    in.inclinationDeg = incl;
    in.longitudeDeg = lon;
    in.retrograde = retro;
    in.startDistanceRt = 3.0;
    return in;
}

static void testEventScales() {
    std::printf("tidal radius and pericentre\n");
    const EventSetup s = setupEvent(inputs(1.0, 1.0, 1e6, 1.0, 0, 0, false));
    // R_sun / (G M / c^2) for 1e6 suns, r_t = R (M_bh/M_star)^(1/3).
    CHECK(std::abs(s.R - 0.47104) < 1e-3, "Sun at 1e6 Msun: R = %.5f M", s.R);
    CHECK(std::abs(s.rt - 100.0 * s.R) < 1e-9 * s.rt, "r_t = 100 R (got %.4f M)", s.rt);
    CHECK(!s.swallowed && std::abs(s.rp - s.rt) < 1e-12, "beta 1: r_p = r_t, not swallowed");
    const EventSetup s8 = setupEvent(inputs(1.0, 1.0, 1e8, 1.0, 0, 0, false));
    CHECK(s8.swallowed && s8.rp < 4.0, "Sun at 1e8 Msun is swallowed whole (r_p = %.3f M)", s8.rp);
    const EventSetup rsg = setupEvent(inputs(18.0, 764.0, 1e8, 1.0, 0, 0, false));
    CHECK(!rsg.swallowed && rsg.rp > 500.0, "red supergiant at 1e8 Msun: r_t = %.1f M", rsg.rt);
    CHECK(s.rStart >= 1.5 * s.rp && std::abs(glm::length(s.x0) - s.rStart) < 1e-3 * s.rStart, "start at %.1f M", glm::length(s.x0));
    CHECK(glm::dot(s.x0, s.v0) < 0.0, "start on the inbound branch");
    CHECK(std::abs(orbitalEnergy(s.x0, s.v0)) < 1e-6 / s.rStart, "start state is parabolic (eps %.2e)", orbitalEnergy(s.x0, s.v0));
    // Orientation conventions: longitude is a right-hand rotation of +X about +Y (the disk's sense of
    // rotation), inclination tilts the orbit plane about the pericentre direction, prograde means the
    // angular momentum is along +Y at zero inclination.
    const EventSetup a = setupEvent(inputs(1.0, 1.0, 1e6, 1.0, 0.0, 90.0, false));
    CHECK(glm::length(a.pHat - dvec3(0, 0, -1)) < 1e-12 && glm::length(a.nHat - dvec3(0, 1, 0)) < 1e-12,
          "longitude 90 deg puts the pericentre at -Z; prograde normal is +Y");
    const EventSetup b = setupEvent(inputs(1.0, 1.0, 1e6, 1.0, 30.0, 0.0, true));
    CHECK(glm::length(b.nHat + dvec3(0, std::cos(glm::radians(30.0)), std::sin(glm::radians(30.0)))) < 1e-12,
          "inclination 30 deg tilts the normal about the pericentre axis; retrograde flips it");
    const dvec3 l = glm::normalize(glm::cross(a.x0, a.v0));
    CHECK(glm::length(l - a.nHat) < 1e-9, "initial angular momentum points along the orbit normal");
}

static void testParabolicPericentre() {
    std::printf("parabolic orbits reach the requested pericentre\n");
    struct Case { double m, r, bh, beta, incl, lon; bool retro; };
    const Case cases[] = {
        {1.0, 1.0, 1e6, 1.0, 20.0, 0.0, false},   {1.0, 1.0, 1e6, 3.0, 60.0, 45.0, true},
        {1.0, 1.0, 1e6, 10.0, 0.0, 200.0, false}, {1.0, 1.0, 1e5, 0.5, 89.0, -30.0, false},
        {18.0, 764.0, 1e8, 1.0, 30.0, 90.0, true}, {1.0, 1.0, 1e7, 2.0, 10.0, 10.0, false},
    };
    for (const Case& c : cases) {
        const EventSetup s = setupEvent(inputs(c.m, c.r, c.bh, c.beta, c.incl, c.lon, c.retro));
        for (double eta : {0.03, 0.005}) {
            dvec3 x = s.x0, v = s.v0;
            const SubcycleConfig cfg{eta, 1 << 20};
            double rmin = glm::length(x);
            dvec3 xmin = x;
            const double chunk = 0.02 * std::pow(s.rp, 1.5);
            integrate(x, v, 1e12, chunk, cfg, [&](double, const dvec3& p, const dvec3& q) {
                if (glm::length(p) < rmin) {
                    rmin = glm::length(p);
                    xmin = p;
                }
                return glm::dot(p, q) > 0.0 && glm::length(p) > 1.2 * s.rp;   // outbound again
            });
            // Refine: the sampled minimum is within one chunk of the true one; with chunk = 0.02 t_p
            // the sampling error is < 0.1%.
            const double err = std::abs(rmin - s.rp) / s.rp;
            const dvec3 L = glm::normalize(glm::cross(x, v));
            const double angN = glm::degrees(std::acos(std::clamp(glm::dot(L, s.nHat), -1.0, 1.0)));
            const double angP = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(xmin), s.pHat), -1.0, 1.0)));
            CHECK(err < 0.01 && angN < 0.01 && angP < 3.0,
                  "M_bh=%.0e beta=%.1f incl=%.0f lon=%.0f %s eta=%.3f: r_min/r_p-1 = %.2e, normal off %.3f deg, peri dir off %.2f deg",
                  c.bh, c.beta, c.incl, c.lon, c.retro ? "retro" : "pro", eta, rmin / s.rp - 1.0, angN, angP);
        }
        const double cosI = glm::dot(s.nHat, dvec3(0, 1, 0));
        CHECK(std::abs(std::abs(cosI) - std::cos(glm::radians(c.incl))) < 1e-9 && ((cosI < 0.0) == c.retro),
              "orbit normal has inclination %.0f deg and %s sense", c.incl, c.retro ? "retrograde" : "prograde");
    }
}

static void testCapture() {
    std::printf("capture radius 4 M for parabolic orbits\n");
    const SubcycleConfig cfg{0.03, 1 << 20};
    for (double rp : {4.2, 5.0}) {
        const double L2 = parabolicL2(rp);
        dvec3 x(rp, 0, 0), v(0, 0, -std::sqrt(L2) / rp);
        double rmin = rp;
        const Fate f = integrate(x, v, 1e5, 0.5, cfg, [&](double, const dvec3& p, const dvec3&) {
            rmin = std::min(rmin, glm::length(p));
            return glm::length(p) > 40.0;
        });
        CHECK(f == Fate::Active && glm::length(x) > 40.0 && rmin > 0.99 * rp, "r_p = %.1f: escapes after pericentre (r_min %.4f)", rp, rmin);
    }
    for (double rp : {3.8, 3.0}) {
        EventInputs in = inputs(1.0, 1.0, 1e6, 1.0, 0, 0, false);
        // Choose beta so that r_p is the requested value.
        const EventSetup ref = setupEvent(in);
        in.beta = ref.rt / rp;
        const EventSetup s = setupEvent(in);
        dvec3 x = s.x0, v = s.v0;
        const Fate f = integrate(x, v, 1e7, 1.0, cfg, [](double, const dvec3&, const dvec3&) { return false; });
        CHECK(s.swallowed && f == Fate::Accreted, "r_p = %.1f: flagged swallowed whole and plunges", rp);
    }
    CHECK(parabolicL2(4.0) == 16.0 && parabolicL2(3.99) < 16.0, "L^2 = 16 at the capture radius");
}

static void testLaneEmden() {
    std::printf("Lane-Emden polytropes\n");
    struct Ref { double n, xi1, omega, rhoRatio; };
    for (const Ref& ref : {Ref{1.5, 3.65375, 2.71406, 5.99071}, Ref{3.0, 6.89685, 2.01824, 54.1825}}) {
        const LaneEmden le = solveLaneEmden(ref.n);
        CHECK(std::abs(le.xi1 - ref.xi1) < 2e-5, "n=%.1f: xi_1 = %.5f (tabulated %.5f)", ref.n, le.xi1, ref.xi1);
        CHECK(std::abs(le.omega - ref.omega) < 2e-5, "n=%.1f: -xi^2 theta'(xi_1) = %.5f (tabulated %.5f)", ref.n, le.omega, ref.omega);
        const double ratio = le.xi1 * le.xi1 * le.xi1 / (3.0 * le.omega);
        CHECK(std::abs(ratio - ref.rhoRatio) < 1e-3 * ref.rhoRatio, "n=%.1f: rho_c / rho_mean = %.4f (tabulated %.4f)", ref.n, ratio, ref.rhoRatio);
        // Mass from integrating the density agrees with the derivative form.
        double m = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            const double xi = le.xi1 * (i + 0.5) / steps;
            m += xi * xi * std::pow(le.thetaAt(xi), ref.n) * le.xi1 / steps;
        }
        CHECK(std::abs(m / le.omega - 1.0) < 1e-4, "n=%.1f: integral of xi^2 theta^n = omega (ratio %.6f)", ref.n, m / le.omega);
        bool mono = true;
        for (size_t i = 1; i < le.mass.size(); ++i) mono = mono && le.mass[i] >= le.mass[i - 1];
        CHECK(mono && le.mass.back() == 1.0, "n=%.1f: enclosed mass is monotone and reaches 1", ref.n);
        // Hydrostatic internal energy: the virial theorem 3 (gamma-1) U + W = 0 with
        // W = -3/(5-n) q^2/R gives U/q = 3/(2(5-n)) q/R for gamma = 5/3.
        const double q = 1e-6, R = 0.5;
        const PolytropeModel pm = makePolytrope(le, q, R);
        double U = 0.0;
        for (int i = 0; i < steps; ++i) {
            const double f0 = static_cast<double>(i) / steps, f1 = static_cast<double>(i + 1) / steps;
            const double xi = le.xiAtMassFraction(0.5 * (f0 + f1));
            U += pm.u(le, xi) * (f1 - f0);
        }
        const double expect = 3.0 / (2.0 * (5.0 - ref.n)) * q / R;
        CHECK(std::abs(U / expect - 1.0) < 2e-3, "n=%.1f: internal energy satisfies the virial theorem (U/expected = %.5f)", ref.n, U / expect);
        // Hydrostatic equilibrium dP/dr = -rho G m(<r)/r^2 at a few radii (finite differences).
        double worst = 0.0;
        for (double frac : {0.2, 0.5, 0.8}) {
            const double xi = frac * le.xi1, h = 1e-3 * le.xi1;
            const double alpha = R / le.xi1;
            auto P = [&](double x) { return pm.pOverRhoC * pm.rhoC * std::pow(le.thetaAt(x), ref.n + 1.0) * q; };
            const double dPdr = (P(xi + h) - P(xi - h)) / (2.0 * h * alpha);
            const double rho = q * pm.rhoC * std::pow(le.thetaAt(xi), ref.n);
            const double r = xi * alpha;
            const double g = q * le.massFractionAt(xi) / (r * r);
            worst = std::max(worst, std::abs(dPdr / (-rho * g) - 1.0));
        }
        CHECK(worst < 2e-3, "n=%.1f: pressure balances self-gravity (worst %.1e)", ref.n, worst);
    }
}

static void testSampling() {
    std::printf("polytrope particle sampling\n");
    for (double n : {1.5, 3.0}) {
        const LaneEmden le = solveLaneEmden(n);
        const int N = 20000;
        const std::vector<dvec3> pts = samplePolytrope(le, N, 7u);
        double worst = 0.0, rmax = 0.0;
        dvec3 c(0.0);
        for (const dvec3& p : pts) {
            c += p;
            rmax = std::max(rmax, glm::length(p));
        }
        c /= static_cast<double>(N);
        for (double f : {0.1, 0.3, 0.5, 0.7, 0.9}) {
            const double r = le.xiAtMassFraction(f) / le.xi1;
            int inside = 0;
            for (const dvec3& p : pts) inside += glm::length(p) < r;
            worst = std::max(worst, std::abs(static_cast<double>(inside) / N - f));
        }
        CHECK(worst < 0.002 && rmax <= 1.0 && glm::length(c) < 0.01,
              "n=%.1f: enclosed-mass profile error %.4f, max radius %.4f, centroid offset %.4f", n, worst, rmax, glm::length(c));
        const std::vector<dvec3> again = samplePolytrope(le, N, 7u);
        CHECK(again == pts, "n=%.1f: sampling is deterministic for a seed", n);
    }
}

// Frozen-in approximation: the intact star arrives at pericentre with the centre-of-mass velocity
// and every element then moves ballistically. About half the mass is bound, the spread of
// specific orbital energy is ~ R / r_t^2, and the bound debris returns on schedule.
static void testFrozenInDisruption() {
    std::printf("ballistic frozen-in disruption (Sun, 1e6 Msun)\n");
    for (double beta : {1.0, 3.0}) {
        const EventSetup s = setupEvent(inputs(1.0, 1.0, 1e6, beta, 20.0, 0.0, false));
        dvec3 X = s.x0, V = s.v0;
        const SubcycleConfig fine{0.002, 1 << 22};
        double rmin = glm::length(X);
        dvec3 Xp = X, Vp = V;
        integrate(X, V, 1e9, 0.002 * std::pow(s.rp, 1.5), fine, [&](double, const dvec3& p, const dvec3& q) {
            if (glm::length(p) < rmin) {
                rmin = glm::length(p);
                Xp = p;
                Vp = q;
            }
            return glm::dot(p, q) > 0.0;
        });
        const LaneEmden le = solveLaneEmden(3.0);
        const int N = 3000;
        const std::vector<dvec3> pts = samplePolytrope(le, N, 11u);
        int bound = 0;
        double emin = 0.0, emax = 0.0;
        std::vector<dvec3> xs, vs;
        for (const dvec3& p : pts) {
            const dvec3 x = Xp + s.R * p;
            const double e = orbitalEnergy(x, Vp);
            bound += e < 0.0;
            emin = std::min(emin, e);
            emax = std::max(emax, e);
            xs.push_back(x);
            vs.push_back(Vp);
        }
        const double frac = static_cast<double>(bound) / N;
        const double de = s.R / (s.rp * s.rp);   // spread at pericentre for the frozen-in star
        CHECK(std::abs(frac - 0.5) < 0.05, "beta=%.0f: bound fraction %.3f", beta, frac);
        CHECK(-emin > 0.7 * de && -emin < 1.3 * de && emax > 0.7 * de && emax < 1.3 * de,
              "beta=%.0f: energy spread [%.2f, %.2f] x R/r_p^2", beta, emin / de, emax / de);
        // The 30 most bound elements come back through pericentre after their Keplerian period
        // 2 pi a^1.5 (a = 1 / (2 |eps|)), the first after t_min; their energies stay put meanwhile.
        const double tmin = 2.0 * kPi * std::pow(1.0 / (2.0 * -emin), 1.5);
        std::vector<std::pair<double, int>> byEnergy;
        for (int i = 0; i < N; ++i) byEnergy.push_back({orbitalEnergy(xs[static_cast<size_t>(i)], vs[static_cast<size_t>(i)]), i});
        std::sort(byEnergy.begin(), byEnergy.end());
        const SubcycleConfig cfg{0.03, 1 << 20};
        int returned = 0, onTime = 0;
        double drift = 0.0;
        const int M = 30;
        for (int k = 0; k < M; ++k) {
            const int i = byEnergy[static_cast<size_t>(k)].second;
            dvec3 x = xs[static_cast<size_t>(i)], v = vs[static_cast<size_t>(i)];
            const double e0 = orbitalEnergy(x, v);
            const double period = 2.0 * kPi * std::pow(1.0 / (2.0 * -e0), 1.5);
            bool wentOut = false, back = false;
            double tBack = 0.0;
            integrate(x, v, 1.2 * period, period / 2000.0, cfg, [&](double t, const dvec3& p, const dvec3&) {
                if (glm::length(p) > 10.0 * s.rp) wentOut = true;
                if (wentOut && glm::length(p) < 3.0 * s.rp) {
                    back = true;
                    tBack = t;
                }
                return back;
            });
            returned += back;
            onTime += back && std::abs(tBack / period - 1.0) < 0.05;
            drift = std::max(drift, std::abs(orbitalEnergy(x, v) - e0) / de);
        }
        CHECK(returned == M && onTime == M && drift < 0.05,
              "beta=%.0f: the %d most bound elements return within 5%% of their period (first at t_min = %.0f M); energy drift %.1e of the spread",
              beta, M, tmin, drift);
    }
}

static void testSubcycleCap() {
    std::printf("sub-cycle cap\n");
    dvec3 x(10.0, 0, 0), v(0, 0, -1.0 / std::sqrt(7.0));
    bool capped = false;
    int used = 0;
    const Fate f = advanceParticle(x, v, 1e4, SubcycleConfig{0.03, 16}, dvec3(0.0), &used, &capped);
    const bool finite = std::isfinite(x.x) && std::isfinite(v.x) && std::isfinite(x.y) && std::isfinite(v.z);
    CHECK(capped && used <= 16 && finite && glm::length(v) <= kMaxSpeed + 1e-12,
          "huge dt with 16 substeps: capped, finite, speed clamped (fate %d)", static_cast<int>(f));
    dvec3 x2(1e9, 0, 0), v2(0.0, 0.0, 0.0);
    advanceParticle(x2, v2, 1e9, SubcycleConfig{0.03, 64});
    CHECK(std::isfinite(x2.x) && x2.x < 1e9, "radial infall from 1e9 M stays finite");
}

int main() {
    testCircularOrbits();
    testIsco();
    testEpicycles();
    testEccentricPrecession();
    testEventScales();
    testParabolicPericentre();
    testCapture();
    testLaneEmden();
    testSampling();
    testFrozenInDisruption();
    testSubcycleCap();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
