#pragma once
// A core-collapse supernova, far from the black hole: the dying star, the collapse of its core, the
// shock's breakout flash, the expanding envelope cooling onto its plateau, and the nebular remnant, a
// pure CPU model in the renderer frame and geometric units of the black hole mass that sets the scene's
// units (G = c = M = 1). SnEvent.cpp draws it (bodies and particles), SnDirector paces and films it,
// tests/sn_tests.cpp checks it. Visuals come first: the numbers follow Type II supernovae (shock
// breakout lasting about the star's light-crossing time, a hydrogen-recombination plateau near 5000 K,
// a nebular phase powered by cobalt-56 decay, a pulsar left in the middle) without hydrodynamics.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

#include "Units.h"

namespace bhs::sn {

struct Params {
    double bhMassSolar = 4.297e6;        // sets the units: the black hole itself is far away
    double starMassSolar = 15.0;
    double starRadiusSolar = 700.0;      // a red supergiant (Betelgeuse-like)
    double starTeffK = 3500.0;
    double siteDistanceM = 2.0e7;        // from the hole: beyond the renderer's lensing (straight rays)
    double siteLongitudeDeg = 0.0;
    double collapseAfterSeconds = 10.0;  // the star is shown this long (event time) before its core collapses
    double shockSpeed = 0.033;           // c: the shock's climb through the envelope
    double ejectaSpeed = 0.03;           // c: the outer ejecta (the photosphere, then the forward shock)
    double nebulaSpeed = 0.024;          // c: the inner ejecta, the remnant's filaments
    double peakTempK = 40000.0;          // the breakout flash (the colour table's limit; really ~2e5 K)
    double plateauDays = 100.0;          // the hydrogen-recombination plateau
    double plateauTempK = 4800.0;
};

struct State {
    double timeM = 0.0;
    double tauS = 0.0;                   // seconds since the breakout (negative before it)
    bool collapsed = false, breakout = false, nebular = false, over = false;
    glm::dvec3 site{0.0};                // the star / the remnant's centre
    // The photosphere (a body): the star, then the flash and the expanding envelope.
    double photoRadiusM = 0.0, photoTempK = 0.0, photoBright = 1.0, photoOpacity = 1.0;
    double envelopeRadiusM = 0.0;        // the outer ejecta (wisps) reach about 1.3 of this
    // The nebular remnant (particles): filaments inside nebulaRadiusM, the forward shock at the outer
    // ejecta's edge (shellRadiusM, where the photosphere was).
    double nebulaRadiusM = 0.0, nebulaBright = 0.0, shellRadiusM = 0.0;
    double pulsarBright = 0.0;           // the neutron star at the centre, once the ejecta have cleared
};

class Model {
public:
    void init(const Params& prm) {
        prm_ = prm;
        mPerM_ = units::metersPerM(prm.bhMassSolar);
        sPerM_ = units::secondsPerM(prm.bhMassSolar);
        R_ = prm.starRadiusSolar * units::kSolarRadius / mPerM_;
        tCrossS_ = R_ * mPerM_ / (prm.shockSpeed * units::kC);
        tCollapseS_ = prm.collapseAfterSeconds;
        tBreakoutS_ = tCollapseS_ + tCrossS_;
        tFlashS_ = R_ * sPerM_;          // the light-crossing time of the star
        // Far enough that the hole is negligible and the renderer's rays straight, whatever mass sets the
        // units: at least thirty times the remnant shot's last distance (under 4 outer radii at 3 years).
        const double reachM = 4.0 * prm.ejectaSpeed * units::kC * 3.2 * 365.25 * 86400.0 / mPerM_;
        const double lon = glm::radians(prm.siteLongitudeDeg);
        site_ = std::max(prm.siteDistanceM, 30.0 * reachM) * glm::dvec3(std::cos(lon), 0.0, -std::sin(lon));
    }

    State at(double tM) const {
        State s;
        s.timeM = tM;
        s.site = site_;
        const double t = tM * sPerM_;
        s.tauS = t - tBreakoutS_;
        s.collapsed = t >= tCollapseS_;
        if (s.tauS < 0.0) {
            s.photoRadiusM = R_;
            s.photoTempK = prm_.starTeffK;
            s.photoBright = 1.0;
            s.envelopeRadiusM = R_;
            return s;
        }
        s.breakout = true;
        const double tau = s.tauS;
        const double days = tau / 86400.0;
        // The flash cools from the breakout over a few light-crossing times onto the plateau; the
        // photosphere rides out with the ejecta.
        s.photoTempK = std::max(prm_.plateauTempK, prm_.peakTempK * std::pow(1.0 + tau / tFlashS_, -0.55));
        s.photoRadiusM = R_ + prm_.ejectaSpeed * tau / sPerM_;
        s.envelopeRadiusM = s.photoRadiusM;
        s.photoBright = 1.0 + 30.0 * std::exp(-tau / tFlashS_);
        // After the plateau the envelope turns transparent over a month; the nebula shows through.
        const double fadeStart = prm_.plateauDays, fadeEnd = prm_.plateauDays + 30.0;
        s.photoOpacity = 1.0 - std::clamp((days - fadeStart) / (fadeEnd - fadeStart), 0.0, 1.0);
        s.nebular = days > 0.8 * fadeStart;
        if (s.nebular) {
            s.nebulaRadiusM = prm_.nebulaSpeed * tau / sPerM_;
            s.shellRadiusM = s.envelopeRadiusM;
            const double reveal = std::clamp((days - 0.8 * fadeStart) / (fadeEnd - 0.8 * fadeStart), 0.0, 1.0);
            // Cobalt-56 (111-day e-folding) lights the filaments; the pulsar's wind keeps a floor.
            s.nebulaBright = reveal * (std::exp(-(days - fadeStart) / 111.0) + 0.08);
            s.pulsarBright = reveal;
        }
        s.over = days > 3.0 * 365.25;
        return s;
    }

    double breakoutTimeM() const { return tBreakoutS_ / sPerM_; }
    double collapseTimeM() const { return tCollapseS_ / sPerM_; }
    double crossingSeconds() const { return tCrossS_; }
    double flashSeconds() const { return tFlashS_; }
    double starRadiusM() const { return R_; }
    glm::dvec3 site() const { return site_; }
    double metersPerM() const { return mPerM_; }
    double secondsPerM() const { return sPerM_; }
    const Params& params() const { return prm_; }

private:
    Params prm_;
    double mPerM_ = 1.0, sPerM_ = 1.0, R_ = 1.0, tCrossS_ = 0.0, tCollapseS_ = 0.0, tBreakoutS_ = 0.0, tFlashS_ = 1.0;
    glm::dvec3 site_{0.0};
};

}  // namespace bhs::sn
