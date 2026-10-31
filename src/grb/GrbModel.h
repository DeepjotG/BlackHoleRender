#pragma once
// A binary neutron-star merger near the black hole: the inspiral driven by gravitational waves, the
// merger, the twin relativistic jets (the gamma-ray burst) and the kilonova (the ejecta glowing as they
// expand). A pure CPU model in the renderer frame and geometric units of the black hole
// (G = c = M_bh = 1): GrbEvent.cpp drives the GPU particles from it, GrbDirector paces and films it,
// tests/grb_tests.cpp checks it.
//
// Physics: a quasi-circular inspiral in the quadrupole approximation, a(t) = a0 (1 - t / tc)^(1/4) with
// tc = (5/256) a0^4 / (eta Mt^3) (Peters 1964), merging when the stars touch; a kilonova photosphere
// expanding homologously at ~0.2 c (slower and redder towards the orbital plane, where the lanthanide-rich
// dynamical ejecta are) whose temperature falls as t^-0.45 from ~1.1e4 K at one day (AT2017gfo) and
// which turns transparent after about a week (its centre of mass keeps the binary's orbit around the hole);
// jets a few degrees wide, launched a moment after the
// merger, coasting at ~c then decelerating as t^(1/4), inside a slower, wider cocoon. The jet spines
// follow null geodesics of the black hole: an ultra-relativistic jet is bent by its gravity like light.
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Units.h"

namespace bhs::grb {

struct Params {
    double bhMassSolar = 4.297e6;
    double nsMassSolar = 1.4;         // each star
    double nsRadiusKm = 12.0;
    double siteRadiusM = 60.0;        // the binary's centre of mass circles the hole at this radius ...
    double siteLongitudeDeg = 200.0;  // ... starting here (about +Y from +X)
    double siteInclinationDeg = 0.0;  // tilt of that orbit from the disk plane (about +X)
    double startSeparationKm = 120.0;
    double jetTiltDeg = 14.0;         // the jet axis, tilted from the direction of the hole (in the orbital plane)
    double jetHalfAngleDeg = 6.0;
    double cocoonHalfAngleDeg = 14.0;
    double cocoonSpeed = 0.6;         // c
    double ejectaSpeed = 0.2;         // c, polar; the equatorial ejecta move at 0.6 of it
    double jetDelaySeconds = 0.05;    // jets launch this long after the merger
    double jetCoastSeconds = 3600.0;  // the heads move at ~c this long, then slow as t^(1/4)
};

struct State {
    double timeM = 0.0;
    bool merged = false;
    double tauM = 0.0;                // time since the merger (M), 0 before it
    glm::dvec3 site{0.0}, siteVel{0.0};        // the binary's centre of mass / the merger remnant
    glm::dvec3 star1{0.0}, star2{0.0}, vel1{0.0}, vel2{0.0};   // before the merger
    double separationM = 0.0, periodM = 0.0;   // of the binary
    glm::dvec3 axis{0.0, 1.0, 0.0};   // the binary's angular momentum: the jet axis
    glm::dvec3 centre{0.0};           // of the ejecta (ballistic from the merger point)
    double knRadiusM = 0.0, knShellFrac = 0.45;   // photosphere radius, thickness of the glowing shell
    double knTempK = 0.0, knTempEqK = 0.0;        // polar and equatorial
    double knTau = 0.0, knBright = 1.0;           // optical depth through the shell, emission multiplier
    double jetHeadM[2] = {0.0, 0.0}, cocoonHeadM[2] = {0.0, 0.0};   // path length along each jet's spine (0: not launched;
                                                                      // a jet aimed into the hole ends at the horizon)
    double jetBright = 0.0, cocoonBright = 0.0;
    double jetTempK = 40000.0, cocoonTempK = 25000.0;
};

constexpr int kSpinePoints = 64;
// A jet's path, sampled at equal path lengths (xyz, w = path length from the launch point).
struct Spine {
    std::array<glm::dvec4, kSpinePoints> p{};
    double length = 0.0;              // of the sampled path (less than asked when captured)
    double minR = 1e300;
    bool captured = false;            // fell into the hole
};

class Model {
public:
    void init(const Params& prm) {
        prm_ = prm;
        mPerM_ = units::metersPerM(prm.bhMassSolar);
        sPerM_ = units::secondsPerM(prm.bhMassSolar);
        m_ = prm.nsMassSolar / prm.bhMassSolar;
        Mt_ = 2.0 * m_;
        Rns_ = prm.nsRadiusKm * 1e3 / mPerM_;
        a0_ = prm.startSeparationKm * 1e3 / mPerM_;
        const double eta = 0.25;
        tc_ = (5.0 / 256.0) * std::pow(a0_, 4.0) / (eta * Mt_ * Mt_ * Mt_);
        aMerge_ = 2.2 * Rns_;
        tMerge_ = aMerge_ >= a0_ ? 0.0 : tc_ * (1.0 - std::pow(aMerge_ / a0_, 4.0));
        omega0_ = std::sqrt(Mt_ / (a0_ * a0_ * a0_));
        // The site's circular orbit (prograde with the disk: at (r, 0, 0) the motion is towards -z).
        Rs_ = std::max(prm.siteRadiusM, 3.5);
        Omega_ = std::pow(Rs_, -1.5);
        psi0_ = glm::radians(prm.siteLongitudeDeg);
        sinI_ = std::sin(glm::radians(prm.siteInclinationDeg));
        cosI_ = std::cos(glm::radians(prm.siteInclinationDeg));
        // The binary's frame: the jet axis tilted from the direction of the hole, in the hole's orbital plane.
        const glm::dvec3 site0 = sitePos(0.0);
        const glm::dvec3 dirHole = -glm::normalize(site0);
        glm::dvec3 perp = glm::cross(dirHole, glm::dvec3(0.0, 1.0, 0.0));
        perp = glm::length(perp) > 1e-9 ? glm::normalize(perp) : glm::dvec3(0.0, 0.0, 1.0);
        const double tilt = glm::radians(prm.jetTiltDeg);
        axis_ = glm::normalize(std::cos(tilt) * dirHole + std::sin(tilt) * perp);
        e1_ = glm::normalize(glm::cross(axis_, glm::dvec3(0.0, 1.0, 0.0)));
        e2_ = glm::cross(axis_, e1_);
        // The merger point and the jets' paths from it (ballistic; aberrated by the site's motion).
        mergeSite_ = sitePos(tMerge_);
        mergeVel_ = siteVelocity(tMerge_);
        spines_[0] = traceSpine(mergeSite_, glm::normalize(axis_ + mergeVel_), 1200.0);
        spines_[1] = traceSpine(mergeSite_, glm::normalize(-axis_ + mergeVel_), 1200.0);
    }

    State at(double tM) const {
        State s;
        s.timeM = tM;
        s.axis = axis_;
        s.site = sitePos(tM);
        s.siteVel = siteVelocity(tM);
        if (tM < tMerge_) {
            const double f = std::max(1.0 - tM / tc_, 1e-12);
            const double a = a0_ * std::pow(f, 0.25);
            const double w = std::sqrt(Mt_ / (a * a * a));
            const double phi = 1.6 * omega0_ * tc_ * (1.0 - std::pow(f, 0.625));
            const glm::dvec3 u = std::cos(phi) * e1_ + std::sin(phi) * e2_;
            const glm::dvec3 v = -std::sin(phi) * e1_ + std::cos(phi) * e2_;
            s.separationM = a;
            s.periodM = 2.0 * 3.14159265358979 / w;
            s.star1 = s.site + 0.5 * a * u;
            s.star2 = s.site - 0.5 * a * u;
            s.vel1 = s.siteVel + 0.5 * a * w * v;
            s.vel2 = s.siteVel - 0.5 * a * w * v;
            s.centre = s.site;
            return s;
        }
        s.merged = true;
        s.tauM = tM - tMerge_;
        s.separationM = 0.0;
        s.centre = s.site;   // the ejecta's centre of mass keeps the binary's orbit about the hole
        const double ts = std::max(s.tauM * sPerM_, 1e-3);   // seconds since the merger
        const double days = ts / 86400.0;
        // Kilonova: homologous expansion; the glowing shell thins as the ejecta turn transparent.
        s.knRadiusM = prm_.ejectaSpeed * s.tauM;
        s.knShellFrac = glm::mix(0.45, 0.25, std::clamp((days - 1.0) / 9.0, 0.0, 1.0));
        s.knTempK = std::clamp(1.1e4 * std::pow(days, -0.45), 1200.0, 40000.0);
        s.knTempEqK = ts < 60.0 ? s.knTempK : std::max(0.6 * s.knTempK, 1200.0);
        s.knTau = std::clamp(30.0 * std::pow(days, -2.0), 0.05, 30.0);
        s.knBright = 0.25 * (1.0 + 2.0 * std::exp(-ts / 2.0));   // the flash of the first seconds
        // Jets: a moment after the merger, coasting at c then decelerating.
        const double tj = s.tauM - prm_.jetDelaySeconds / sPerM_;
        if (tj > 0.0) {
            const double tjs = tj * sPerM_;
            const double coastM = prm_.jetCoastSeconds / sPerM_;
            const double head = tjs < prm_.jetCoastSeconds ? tj : coastM * std::pow(tjs / prm_.jetCoastSeconds, 0.25);
            for (int w = 0; w < 2; ++w) {
                s.jetHeadM[w] = std::min(head, spines_[w].length);
                s.cocoonHeadM[w] = std::min(prm_.cocoonSpeed * tj, 0.9 * s.jetHeadM[w]);
            }
            s.jetBright = 0.4 + 1.3 / (1.0 + tjs / 60.0);   // bright for the first minute, then the afterglow
            s.cocoonBright = 0.12 * s.jetBright;
        }
        return s;
    }

    double mergeTimeM() const { return tMerge_; }
    double coalescenceTimeM() const { return tc_; }
    double initialSeparationM() const { return a0_; }
    double nsRadiusM() const { return Rns_; }
    double metersPerM() const { return mPerM_; }
    double secondsPerM() const { return sPerM_; }
    const Params& params() const { return prm_; }
    const Spine& spine(int which) const { return spines_[which]; }   // 0: along the axis, 1: opposite
    glm::dvec3 e1() const { return e1_; }
    glm::dvec3 e2() const { return e2_; }
    glm::dvec3 axis() const { return axis_; }

    glm::dvec3 sitePos(double tM) const {
        const double psi = psi0_ + Omega_ * tM;
        return Rs_ * glm::dvec3(std::cos(psi), std::sin(psi) * sinI_, -std::sin(psi) * cosI_);
    }
    glm::dvec3 siteVelocity(double tM) const {
        const double psi = psi0_ + Omega_ * tM;
        return Rs_ * Omega_ * glm::dvec3(-std::sin(psi), std::cos(psi) * sinI_, -std::cos(psi) * cosI_);
    }

    // A null geodesic from x0 in the direction d0 (x'' = -3 h^2 x / r^5, exact for the path's shape), traced
    // up to maxLen or into the horizon, then sampled at kSpinePoints equal path lengths over the path it
    // actually has (a jet aimed into the hole gets its few M to the horizon finely sampled).
    static Spine traceSpine(const glm::dvec3& x0, const glm::dvec3& d0, double maxLen) {
        Spine sp;
        glm::dvec3 x = x0, v = glm::normalize(d0);
        const glm::dvec3 hv = glm::cross(x, v);
        const double h2 = glm::dot(hv, hv);
        const auto accel = [h2](const glm::dvec3& p) {
            const double r2 = glm::dot(p, p), r = std::sqrt(r2);
            return p * (-3.0 * h2 / (r2 * r2 * r));
        };
        std::vector<glm::dvec4> path;   // xyz, w = path length
        path.reserve(4096);
        path.emplace_back(x, 0.0);
        sp.minR = glm::length(x);
        double len = 0.0;
        for (int it = 0; it < 400000 && len < maxLen; ++it) {
            const double r = glm::length(x);
            if (r < 2.05) {
                sp.captured = true;
                break;
            }
            const double ds = std::clamp(0.01 * r, 1e-4, maxLen / (2.0 * kSpinePoints));
            const glm::dvec3 k1v = accel(x), k1x = v;
            const glm::dvec3 k2v = accel(x + 0.5 * ds * k1x), k2x = v + 0.5 * ds * k1v;
            const glm::dvec3 k3v = accel(x + 0.5 * ds * k2x), k3x = v + 0.5 * ds * k2v;
            const glm::dvec3 k4v = accel(x + ds * k3x), k4x = v + ds * k3v;
            const glm::dvec3 xPrev = x;
            x += (ds / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
            v += (ds / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
            v = glm::normalize(v);
            len += glm::length(x - xPrev);
            sp.minR = std::min(sp.minR, glm::length(x));
            path.emplace_back(x, len);
        }
        sp.length = std::min(len, maxLen);
        const double step = sp.length / (kSpinePoints - 1);
        size_t j = 0;
        for (int k = 0; k < kSpinePoints; ++k) {
            const double want = k * step;
            while (j + 1 < path.size() && path[j + 1].w < want) ++j;
            if (j + 1 < path.size()) {
                const double t = (want - path[j].w) / std::max(path[j + 1].w - path[j].w, 1e-300);
                sp.p[static_cast<size_t>(k)] = glm::dvec4(glm::mix(glm::dvec3(path[j]), glm::dvec3(path[j + 1]), std::clamp(t, 0.0, 1.0)), want);
            } else {
                sp.p[static_cast<size_t>(k)] = glm::dvec4(glm::dvec3(path.back()), want);
            }
        }
        return sp;
    }

private:
    Params prm_;
    double mPerM_ = 1.0, sPerM_ = 1.0;
    double m_ = 0.0, Mt_ = 0.0, Rns_ = 0.0, a0_ = 0.0, tc_ = 0.0, aMerge_ = 0.0, tMerge_ = 0.0, omega0_ = 0.0;
    double Rs_ = 60.0, Omega_ = 0.0, psi0_ = 0.0, sinI_ = 0.0, cosI_ = 1.0;
    glm::dvec3 axis_{0.0, 1.0, 0.0}, e1_{1.0, 0.0, 0.0}, e2_{0.0, 0.0, 1.0};
    glm::dvec3 mergeSite_{0.0}, mergeVel_{0.0};
    Spine spines_[2];
};

}  // namespace bhs::grb
