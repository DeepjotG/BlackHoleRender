#include "grb/GrbDirector.h"

#include <algorithm>
#include <cmath>

namespace bhs {
namespace {

constexpr double kPi = 3.14159265358979;

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

}  // namespace

void GrbDirector::start(const grb::Model& model) {
    stage_ = kInspiral;
    camera_ = true;
    stageSeconds_ = overSeconds_ = 0.0;
    drift_ = 0.0;
    view_ = vClose;
    cut_ = -1.0;
    fade_ = 0.0;
    sPerM_ = model.secondsPerM();
    a0_ = model.initialSeparationM();
    siteR_ = model.params().siteRadiusM;
    const grb::State s0 = model.at(0.0);
    period0_ = std::max(s0.periodM, 1e-12);
    pace_ = period0_ / 0.25;
    // The close shots look at the binary from its far side, so the black hole looms behind it, from a
    // little above the binary's orbital plane and off its axis (the jets will cross the frame).
    const glm::dvec3 dirHole = -glm::normalize(s0.site);
    closeDir_ = glm::normalize(-0.85 * dirHole + 0.45 * model.e1() + 0.3 * glm::dvec3(0.0, 1.0, 0.0));
}

void GrbDirector::setStage(Stage s) {
    stage_ = s;
    stageSeconds_ = 0.0;
}

const char* GrbDirector::title() const {
    switch (stage_) {
    case kInspiral: return "1/5  Two neutron stars spiral in (slow motion)";
    case kMerger: return "2/5  Merger: the gamma-ray burst (slow motion)";
    case kBurst: return "3/5  The jets race out, the kilonova swells (time-lapse)";
    case kAfterglow: return "4/5  The kilonova engulfs the black hole";
    case kDays: return "5/5  The days after (time-lapse, from far out)";
    case kOver: return "The kilonova has faded";
    default: return "";
    }
}

const char* GrbDirector::caption() const {
    switch (stage_) {
    case kInspiral:
        return "Each star packs 1.4 Suns into a 24 km ball. Gravitational waves drain their orbit, so the whirl "
               "tightens and quickens (the chirp) until they touch, a thousand times a second.";
    case kMerger:
        return "In a millisecond they become one. A fiftieth of a solar mass of neutron-rich matter is flung out at "
               "a fifth of the speed of light, and twin jets punch out along the spin axis: the gamma-ray burst.";
    case kBurst:
        return "The jets, a few degrees wide and moving at almost the speed of light, race away; the one aimed past "
               "the black hole is bent by its gravity. The ejecta glow white-hot, forging gold and platinum.";
    case kAfterglow:
        return "The jets fade into an afterglow while the ejecta, still white-hot, sweep over the black hole and then "
               "the camera.";
    case kDays:
        return "The black hole is inside the cloud now. Over the days that follow the ejecta cool from white through "
               "yellow to red and turn transparent, carrying the gold and platinum they forged out into space.";
    case kOver: return "The ejecta have dispersed; the black hole is quiet again.";
    default: return "";
    }
}

GrbDirector::Pose GrbDirector::viewPose(View v, const Input& in) const {
    const grb::State& s = *in.state;
    switch (v) {
    case vClose: return {s.centre + closeDir_ * (4.0 * a0_), s.centre};
    case vMedium: {
        // The flash from outside; then a steady pull-back that keeps the swelling ball at a constant size
        // (14 degrees) while the black hole behind it shrinks: the ejecta grow five orders of magnitude in
        // a minute, which no fixed vantage can follow.
        const double d = std::max(200.0 * a0_, 8.0 * s.knRadiusM);
        return {s.centre + closeDir_ * d, s.centre};
    }
    case vFar: return {s.centre + closeDir_ * (0.1 * siteR_), s.centre};   // unused
    case vPull: return {s.centre + pullDir_ * (8.0 * std::max(s.knRadiusM, 1e-6)), s.centre};
    case vWide: {
        // Framing the merger site and the black hole: from the side of the line between them, above the
        // orbital plane, drifting slowly round.
        const glm::dvec3 site = s.centre;
        const glm::dvec3 sHat = glm::normalize(site);
        const glm::dvec3 up(0.0, 1.0, 0.0);
        glm::dvec3 side = glm::cross(up, sHat);
        side = glm::length(side) > 1e-9 ? glm::normalize(side) : glm::dvec3(0.0, 0.0, 1.0);
        const double d = 3.5 * glm::length(site);
        const double az = glm::radians(drift_);
        const glm::dvec3 dir = glm::normalize((0.85 * std::cos(az)) * side + (0.85 * std::sin(az)) * sHat + 0.5 * up + 0.2 * sHat);
        return {0.5 * site + dir * d, 0.5 * site};
    }
    }
    return {viewPos_, viewTarget_};
}

void GrbDirector::update(const Input& in) {
    if (stage_ == kIdle || !in.state || !in.model) return;
    const grb::State& s = *in.state;
    const double dt = std::max(in.dt, 0.0);
    stageSeconds_ += dt;
    const double camDist = glm::length(in.cameraPos - s.centre);

    // ---- stages
    switch (stage_) {
    case kInspiral:
        if (s.merged) setStage(kMerger);
        break;
    case kMerger:
        if (s.knRadiusM > 0.45 * 200.0 * a0_ || stageSeconds_ > 14.0) setStage(kBurst);
        break;
    case kBurst:
        if (s.knRadiusM > 0.3 * siteR_ || s.tauM * sPerM_ > 86400.0) {
            setStage(kAfterglow);
            // The ejecta reach the camera in about 50 s of watching.
            holdPace_ = std::max((camDist - s.knRadiusM) / (in.model->params().ejectaSpeed * 50.0), 1e-6);
        }
        break;
    case kAfterglow:
        if (s.knRadiusM > 0.8 * camDist) {   // the shell is about to reach the camera: cut to far out for the days after
            setStage(kDays);
            daysPace0_ = pace_;
            const glm::dvec3 away = in.cameraPos - s.centre;
            pullDir_ = glm::length(away) > 1e-9 ? glm::normalize(away) : glm::dvec3(0.0, 1.0, 0.0);
        }
        // The shell reaches the camera about 50 s after this stage began, however the camera and the
        // ejecta's centre move meanwhile.
        holdPace_ = std::max((camDist - s.knRadiusM) / (in.model->params().ejectaSpeed * std::max(50.0 - stageSeconds_, 4.0)), 1e-6);
        break;
    case kDays:
        if (s.tauM * sPerM_ > 10.0 * 86400.0 || stageSeconds_ > 32.0) setStage(kOver);
        break;
    case kOver: overSeconds_ += dt; break;
    default: break;
    }

    // ---- pace of the event clock (M per real second)
    double target = pace_;
    switch (stage_) {
    case kInspiral: {
        // One orbit every 0.25 s at the start, quickening to about 8 a second as the orbit shrinks: the chirp
        // stays visible (real time would be a thousand a second at the end). About 19 s in all.
        const double P = std::max(s.periodM, 1e-12);
        target = (P / 0.25) * std::pow(period0_ / P, 0.3);
        break;
    }
    case kMerger: target = 0.02 / sPerM_; break;                                   // 50x slow motion
    case kBurst: target = std::min(0.02 * std::exp2(stageSeconds_ / 1.5), 2000.0) / sPerM_; break;
    case kAfterglow: target = holdPace_; break;
    case kDays: target = daysPace0_ * std::exp2(stageSeconds_ / 2.5); break;   // ten days in about 23 s
    case kOver: target = 0.0; break;
    default: break;
    }
    if (target > 0.0) {
        const double k = 1.0 - std::exp(-dt / 0.35);
        pace_ = std::exp(std::log(std::max(pace_, 1e-12)) + (std::log(target) - std::log(std::max(pace_, 1e-12))) * k);
    } else {
        pace_ = 0.0;
    }
    if (stage_ == kBurst || stage_ == kAfterglow) drift_ += 1.5 * dt;

    // ---- shots: cuts through black (0.8 s; the view switches at the black frame)
    View want = vClose;
    switch (stage_) {
    case kInspiral: want = vClose; break;
    case kMerger: want = s.knRadiusM > 0.25 * 4.0 * a0_ ? vMedium : vClose; break;   // the flash engulfs the close shot
    case kBurst: want = 8.0 * s.knRadiusM >= 3.5 * glm::length(s.centre) ? vWide : vMedium; break;   // the pull-back has reached the wide shot
    case kAfterglow: want = vWide; break;
    case kDays:
    case kOver: want = vPull; break;
    default: break;
    }
    if (want != view_ && cut_ < 0.0) cut_ = 0.0;
    if (cut_ >= 0.0) {
        cut_ += dt;
        if (cut_ >= 0.4 && view_ != want) view_ = want;
        fade_ = cut_ < 0.4 ? cut_ / 0.4 : std::max(0.0, 1.0 - (cut_ - 0.4) / 0.4);
        if (cut_ >= 0.8) {
            cut_ = -1.0;
            fade_ = 0.0;
        }
    }
    if (stage_ == kOver) fade_ = std::max(fade_, smooth01(overSeconds_ / 1.0));   // and out
    const Pose cur = viewPose(view_, in);
    viewPos_ = cur.pos;
    viewTarget_ = cur.target;
}

}  // namespace bhs
