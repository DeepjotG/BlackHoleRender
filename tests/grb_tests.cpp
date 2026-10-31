// Tests of the neutron-star merger model (src/grb/GrbModel.h).
#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>

#include "grb/GrbModel.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

constexpr double kTwoPi = 6.283185307179586;

void testInspiral() {
    bhs::grb::Params p;
    bhs::grb::Model m;
    m.init(p);
    const double sPerM = m.secondsPerM(), mPerM = m.metersPerM();
    // From 120 km two 1.4 solar-mass stars merge in about a second (Peters' formula), after dozens of orbits.
    const double tMergeS = m.mergeTimeM() * sPerM;
    std::printf("  merger after %.3f s; initial period %.1f ms\n", tMergeS, m.at(0.0).periodM * sPerM * 1e3);
    CHECK(tMergeS > 0.5 && tMergeS < 1.5);
    CHECK(std::abs(m.at(0.0).periodM * sPerM - 0.0136) < 0.002);
    // The analytic a(t) and phase agree with the integrated quadrupole formula da/dt = -(64/5) eta Mt^3 / a^3.
    const double Mt = 2.0 * p.nsMassSolar / p.bhMassSolar, eta = 0.25;
    double a = p.startSeparationKm * 1e3 / mPerM, phi = 0.0, t = 0.0;
    const double dt = m.mergeTimeM() / 200000.0;
    double maxErrA = 0.0, maxErrPhi = 0.0;
    const double tEnd = 0.95 * m.mergeTimeM();
    while (t < tEnd) {
        const double w = std::sqrt(Mt / (a * a * a));
        a += -(64.0 / 5.0) * eta * Mt * Mt * Mt / (a * a * a) * dt;
        phi += w * dt;
        t += dt;
        const bhs::grb::State s = m.at(t);
        maxErrA = std::max(maxErrA, std::abs(s.separationM - a) / a);
        const glm::dvec3 u = glm::normalize(s.star1 - s.site);
        const double phiModel = std::atan2(glm::dot(u, m.e2()), glm::dot(u, m.e1()));
        double d = std::fmod(phiModel - phi, kTwoPi);
        if (d > 3.14159) d -= kTwoPi;
        if (d < -3.14159) d += kTwoPi;
        maxErrPhi = std::max(maxErrPhi, std::abs(d));
    }
    std::printf("  separation error %.2e, phase error %.3f rad after %.0f orbits\n", maxErrA, maxErrPhi, phi / kTwoPi);
    CHECK(maxErrA < 1e-3);
    CHECK(maxErrPhi < 0.05);
    CHECK(phi / kTwoPi > 40.0);
    // Symmetry about the centre of mass, which moves with the site's circular orbit.
    const bhs::grb::State s = m.at(0.3 * m.mergeTimeM());
    CHECK(glm::length(0.5 * (s.star1 + s.star2) - s.site) < 1e-9 * glm::length(s.site));
    CHECK(glm::length(0.5 * (s.vel1 + s.vel2) - s.siteVel) < 1e-9);
    CHECK(std::abs(glm::length(s.site) - p.siteRadiusM) < 1e-9);
    CHECK(std::abs(glm::length(s.siteVel) - std::pow(p.siteRadiusM, -0.5)) < 1e-9);
    CHECK(!s.merged && s.knRadiusM == 0.0 && s.jetHeadM[0] == 0.0 && s.jetHeadM[1] == 0.0);
}

void testAftermath() {
    bhs::grb::Params p;
    bhs::grb::Model m;
    m.init(p);
    const double sPerM = m.secondsPerM();
    const double tm = m.mergeTimeM();
    const bhs::grb::State early = m.at(tm + 1.0 / sPerM), hour = m.at(tm + 3600.0 / sPerM), day = m.at(tm + 86400.0 / sPerM),
                          week = m.at(tm + 10.0 * 86400.0 / sPerM);
    CHECK(early.merged && hour.merged);
    // The kilonova expands at the ejecta speed and cools from the blue to the red.
    CHECK(std::abs(hour.knRadiusM - p.ejectaSpeed * 3600.0 / sPerM) < 1e-6 * hour.knRadiusM);
    CHECK(early.knTempK >= hour.knTempK && hour.knTempK > day.knTempK && day.knTempK > week.knTempK);
    CHECK(std::abs(day.knTempK - 1.1e4) < 1.0);
    CHECK(week.knTempK < 4500.0 && week.knTempK >= 1200.0);
    CHECK(early.knTau >= day.knTau && week.knTau < day.knTau);
    CHECK(early.knBright > hour.knBright);
    // The jets launch a moment after the merger, coast at c, then slow down.
    const bhs::grb::State beforeJet = m.at(tm + 0.5 * p.jetDelaySeconds / sPerM);
    CHECK(beforeJet.jetHeadM[0] == 0.0 && beforeJet.jetHeadM[1] == 0.0);
    const double t10 = 10.0;   // s after the merger
    const bhs::grb::State ten = m.at(tm + t10 / sPerM);
    CHECK(std::abs(ten.jetHeadM[0] - (t10 - p.jetDelaySeconds) / sPerM) < 1e-9 * ten.jetHeadM[0]);
    CHECK(day.jetHeadM[0] > hour.jetHeadM[0] && day.jetHeadM[0] < 3.0 * hour.jetHeadM[0]);   // t^(1/4) after an hour
    CHECK(ten.cocoonHeadM[0] < ten.jetHeadM[0] && ten.cocoonHeadM[0] > 0.0);
    CHECK(ten.jetBright > hour.jetBright && hour.jetBright > 0.4);
    std::printf("  jet head after an hour %.1f M, after a day %.1f M; kilonova after an hour %.1f M, %.0f K\n", hour.jetHeadM[0],
                day.jetHeadM[0], hour.knRadiusM, hour.knTempK);
}

void testSpines() {
    // A jet aimed straight at the hole falls in. One tilted by 14 degrees from 60 M is aberrated by the
    // binary's orbital motion (0.13 c, about 7 degrees more), so it passes at about 60 sin(21 deg) = 21 M
    // (a little closer: it is pulled in) and is bent by roughly 4 / b.
    bhs::grb::Params p;
    p.jetTiltDeg = 0.0;
    bhs::grb::Model straight;
    straight.init(p);
    CHECK(straight.spine(0).captured);
    CHECK(!straight.spine(1).captured);
    // A captured spine keeps its path to the horizon, finely sampled, and that jet stops there.
    const bhs::grb::Spine& in = straight.spine(0);
    std::printf("  captured jet: %.1f M of path to the horizon, closest %.2f M\n", in.length, in.minR);
    CHECK(in.length > 40.0 && in.length < 400.0);   // more than the straight 58 M: it winds in round the photon sphere
    CHECK(glm::length(glm::dvec3(in.p[bhs::grb::kSpinePoints - 1])) < 2.2);
    const bhs::grb::State late = straight.at(straight.mergeTimeM() + 3600.0 / straight.secondsPerM());
    CHECK(std::abs(late.jetHeadM[0] - in.length) < 1e-9 && late.jetHeadM[1] > 100.0);
    // An inclined site orbit stays circular at the same speed.
    bhs::grb::Params pi = p;
    pi.siteInclinationDeg = 45.0;
    bhs::grb::Model inclined;
    inclined.init(pi);
    const bhs::grb::State si = inclined.at(0.1 * inclined.mergeTimeM());
    CHECK(std::abs(glm::length(si.site) - p.siteRadiusM) < 1e-9 && std::abs(glm::length(si.siteVel) - std::pow(p.siteRadiusM, -0.5)) < 1e-9);
    CHECK(std::abs(glm::dot(si.site, si.siteVel)) < 1e-9);
    CHECK(std::abs(si.site.y) > 1.0);
    p.jetTiltDeg = 14.0;
    bhs::grb::Model tilted;
    tilted.init(p);
    const bhs::grb::Spine& sp = tilted.spine(0);
    CHECK(!sp.captured);
    CHECK(sp.minR > 16.0 && sp.minR < 22.0);
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(sp.p[1]) - glm::dvec3(sp.p[0]));
    const glm::dvec3 d1 = glm::normalize(glm::dvec3(sp.p[bhs::grb::kSpinePoints - 1]) - glm::dvec3(sp.p[bhs::grb::kSpinePoints - 2]));
    const double bend = std::acos(std::clamp(glm::dot(d0, d1), -1.0, 1.0));
    const double expected = 4.0 / sp.minR;
    std::printf("  tilted jet: closest %.1f M, bent by %.1f deg (4/b: %.1f deg)\n", sp.minR, glm::degrees(bend), glm::degrees(expected));
    CHECK(bend > 0.6 * expected && bend < 1.6 * expected);
    // Sampled at equal path lengths out to the asked length.
    CHECK(std::abs(sp.length - 1200.0) < 1e-6);
    for (int k = 1; k < bhs::grb::kSpinePoints; ++k) {
        const double seg = glm::length(glm::dvec3(sp.p[static_cast<size_t>(k)]) - glm::dvec3(sp.p[static_cast<size_t>(k - 1)]));
        CHECK(seg > 0.0 && seg <= 1200.0 / (bhs::grb::kSpinePoints - 1) + 1e-6);
    }
    // The opposite jet leaves almost straight.
    CHECK(tilted.spine(1).minR > 50.0);
}

}  // namespace

int main() {
    testInspiral();
    testAftermath();
    testSpines();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("grb_tests: all passed\n");
    return 0;
}
