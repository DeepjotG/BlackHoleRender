#include "sn/SnDirector.h"

#include <algorithm>
#include <cmath>

namespace bhs {
namespace {

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// The fireball's apparent radius: the star's from 3.5 radii away, growing with the ejecta (slower than
// they do, so the camera pulls back as they swell) to most of the frame's half-height (the vertical field
// of view is 60 degrees), its fingers still inside; the remnant whole at the end.
const double kTheta0 = std::asin(1.0 / 3.5);
constexpr double kThetaMax = 24.0 * 3.14159265358979 / 180.0;
constexpr double kThetaEnd = 17.0 * 3.14159265358979 / 180.0;

}  // namespace

void SnDirector::start(const sn::Model& model) {
    stage_ = kStar;
    camera_ = true;
    R_ = model.starRadiusM();
    sPerM_ = model.secondsPerM();
    crossing_ = model.crossingSeconds();
    pace_ = 1.0 / sPerM_;   // real time
    stageSeconds_ = overSeconds_ = remnantSeconds_ = 0.0;
    orbit_ = 30.0;
    fade_ = 0.0;
}

void SnDirector::setStage(Stage s) {
    stage_ = s;
    stageSeconds_ = 0.0;
}

const char* SnDirector::title() const {
    switch (stage_) {
    case kStar: return "1/5  A dying star";
    case kCollapse: return "2/5  Core collapse (time-lapse)";
    case kBreakout: return "3/5  Shock breakout";
    case kExpansion: return "4/5  The star blows apart (time-lapse)";
    case kRemnant: return "5/5  The remnant (years in seconds)";
    case kOver: return "The remnant fades";
    default: return "";
    }
}

const char* SnDirector::caption() const {
    switch (stage_) {
    case kStar:
        return "A red supergiant of fifteen Suns, seven hundred times the Sun's width, its surface boiling in giant "
               "convection cells. Its core has burnt through to iron and can no longer hold itself up.";
    case kCollapse:
        return "The core collapses into a neutron star in a fraction of a second and rebounds. The shock wave takes "
               "half a day to climb out through the envelope; from outside nothing shows yet.";
    case kBreakout:
        return "The shock reaches the surface: for about half an hour the star's whole face flashes at hundreds of "
               "thousands of degrees, briefly outshining its galaxy, and begins to fly apart.";
    case kExpansion:
        return "The envelope expands at ten thousand kilometres a second, plumes of hot gas boiling through it, its "
               "surface cooling to a golden 5,000 degrees as the hydrogen recombines; it shines like that for months.";
    case kRemnant:
        return "The ejecta thin out and turn transparent. Inside, filaments of hydrogen and oxygen glow on the decay of "
               "the cobalt forged in the blast, and the pulsar left by the core lights its wind nebula at the centre.";
    case kOver: return "The remnant will thin for thousands of years into the clouds that form new stars and planets.";
    default: return "";
    }
}

void SnDirector::update(const Input& in) {
    if (stage_ == kIdle || !in.state || !in.model) return;
    const sn::State& s = *in.state;
    const double dt = std::max(in.dt, 0.0);
    stageSeconds_ += dt;

    switch (stage_) {
    case kStar: if (s.collapsed) setStage(kCollapse); break;
    case kCollapse: if (s.breakout) setStage(kBreakout); break;
    case kBreakout: if (s.tauS > 3600.0) setStage(kExpansion); break;
    case kExpansion: if (s.nebular) setStage(kRemnant); break;
    case kRemnant:
        remnantSeconds_ += dt;
        if (s.over || stageSeconds_ > 40.0) setStage(kOver);
        break;
    case kOver: overSeconds_ += dt; break;
    default: break;
    }

    // ---- pace (M per real second)
    double target = pace_;
    switch (stage_) {
    case kStar: target = 1.0 / sPerM_; break;                                          // real time
    case kCollapse: target = (crossing_ / 6.0) / sPerM_; break;                        // the climb in 6 s
    case kBreakout: target = 360.0 / sPerM_; break;                                    // the hour in 10 s
    case kExpansion: target = std::min(360.0 * std::exp2(stageSeconds_ / 2.0), 8.0e5) / sPerM_; break;   // weeks in 28 s
    case kRemnant:   // the envelope's turning transparent held about 10 s, then years in about 17 s
        target = std::min(stageSeconds_ < 8.0 ? 4.0e5 : 4.0e5 * std::exp2((stageSeconds_ - 8.0) / 3.0), 1.0e8) / sPerM_;
        break;
    case kOver: target = 0.0; break;
    default: break;
    }
    if (target > 0.0) {
        const double k = 1.0 - std::exp(-dt / 0.35);
        pace_ = std::exp(std::log(std::max(pace_, 1e-12)) + (std::log(target) - std::log(std::max(pace_, 1e-12))) * k);
    } else {
        pace_ = 0.0;
    }

    // ---- the shot: a slow orbit, a little above the star's equator; the distance keeps the fireball at
    // its apparent size (see kTheta0)
    orbit_ += (stage_ == kStar || stage_ == kCollapse ? 3.0 : 2.0) * dt;
    const double R = std::max(s.envelopeRadiusM, R_);
    double theta = std::min(kTheta0 * std::pow(R / R_, 0.35), kThetaMax);
    if (stage_ == kRemnant || stage_ == kOver) theta += (kThetaEnd - theta) * smooth01((remnantSeconds_ - 10.0) / 14.0);
    const double az = glm::radians(orbit_), el = glm::radians(16.0);
    const glm::dvec3 dir(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
    viewTarget_ = s.site;
    viewPos_ = s.site + dir * (R / std::sin(theta));
    fade_ = stage_ == kOver ? smooth01(overSeconds_ / 1.0) : 0.0;
}

}  // namespace bhs
