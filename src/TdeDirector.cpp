#include "TdeDirector.h"

#include <algorithm>
#include <cmath>

#include "GasParticles.h"

namespace bhs {
namespace {

constexpr double kPi = 3.14159265358979;

// The slow-motion pace: an orbit at the circularisation radius (2 r_p) in 8 s.
double wRetOf(double rp) { return 2.0 * kPi * std::pow(2.0 * rp, 1.5) / 8.0; }

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Unit vector between unit vectors a and b; nearly opposite ones turn through `via`.
glm::dvec3 slerpDir(const glm::dvec3& a, const glm::dvec3& b, double s, const glm::dvec3& via) {
    const double c = std::clamp(glm::dot(a, b), -1.0, 1.0);
    if (c < -0.999) return s < 0.5 ? slerpDir(a, via, 2.0 * s, via) : slerpDir(via, b, 2.0 * s - 1.0, via);
    const double th = std::acos(c);
    if (th < 1e-5) return glm::normalize(glm::mix(a, b, s));
    return (std::sin((1.0 - s) * th) * a + std::sin(s * th) * b) / std::sin(th);
}

}  // namespace

void TdeDirector::start(const glm::dvec3& pos, const glm::dvec3& vel) {
    stage_ = kApproach;
    camera_ = true;
    tPeri_ = tFirst_ = -1.0;
    pace_ = 1.0;
    stageSeconds_ = waitSeconds_ = returnSeconds_ = diskSeconds_ = 0.0;
    disk_ = drift_ = 0.0;
    accretedAtStream_ = 0.0;
    earlyReturnView_ = false;
    view_ = vRide;
    cut_ = -1.0;
    fade_ = 0.0;
    streamLen_ = 0.0;
    rideInit_ = false;
    rideLook_ = 0.15;
    // Orbit frame: the normal, and the pericentre direction from the Laplace-Runge-Lenz vector.
    const glm::dvec3 L = glm::cross(pos, vel);
    n_ = glm::length(L) > 0.0 ? glm::normalize(L) : glm::dvec3(0.0, 1.0, 0.0);
    glm::dvec3 A = glm::cross(vel, L) - pos / std::max(glm::length(pos), 1e-9);
    A -= n_ * glm::dot(A, n_);
    glm::dvec3 inPlane = pos - n_ * glm::dot(pos, n_);
    p_ = glm::length(A) > 1e-9 ? glm::normalize(A) : glm::normalize(glm::length(inPlane) > 0.0 ? inPlane : glm::dvec3(1.0, 0.0, 0.0));
    q_ = glm::cross(n_, p_);
    align_ = smooth01((std::abs(n_.y) - std::cos(25.0 * kPi / 180.0)) / (std::cos(10.0 * kPi / 180.0) - std::cos(25.0 * kPi / 180.0)));
}

void TdeDirector::setStage(Stage s) {
    stage_ = s;
    stageSeconds_ = 0.0;
}

const char* TdeDirector::title() const {
    switch (stage_) {
    case kApproach: return "1/5  Falling in";
    case kTear: return "2/5  Torn apart (slow motion)";
    case kStream: return "3/5  The stream flies out (time-lapse)";
    case kReturn: return "4/5  Falling back";
    case kDisk: return "5/5  Feeding the black hole";
    case kSwallowed: return "Swallowed whole";
    default: return "";
    }
}

const char* TdeDirector::caption() const {
    switch (stage_) {
    case kApproach:
        return "The black hole pulls harder on the star's near side than on its far side. Close in, that difference "
               "(the tide) beats the star's own gravity.";
    case kTear: return "The tides stretch the star into a long, thin stream of gas: spaghettification.";
    case kStream:
        return "About half of the gas is fast enough to escape for good. The rest is still bound to the black hole and "
               "swings back. Weeks pass in seconds here.";
    case kReturn:
        return "The bound gas comes back. Relativity has turned its orbit, so it slams into the stream still falling in "
               "and heats up to tens of thousands of degrees.";
    case kDisk:
        return "The hot gas circles the black hole in a ring that spreads into a disk and spirals inwards: it is "
               "eating the star. Clear ends the event.";
    case kSwallowed: return "This star came too close for its size: it falls in whole before the tides can tear it apart.";
    default: return "";
    }
}

TdeDirector::Pose TdeDirector::holePose(double dist, double elevDeg, double azDeg, const glm::dvec3& target) const {
    const double el = elevDeg * kPi / 180.0, az = azDeg * kPi / 180.0;
    const glm::dvec3 dir = std::cos(el) * (std::cos(az) * p_ + std::sin(az) * q_) + std::sin(el) * n_;
    return {dir * dist, target};
}

TdeDirector::Pose TdeDirector::ridePose(const Input& in) const {
    // Riding along just behind, above and outside the star, looking along its path: the star in front, the
    // black hole looming behind it.
    return {in.pos + rideDir_ * rideDist_, glm::mix(in.pos, glm::dvec3(0.0), rideLook_)};
}

TdeDirector::Pose TdeDirector::viewPose(View v, const Input& in) const {
    switch (v) {
    case vRide: return ridePose(in);
    case vTear:   // kept for scenarios whose star is large next to the hole (not used by default)
        return holePose(1.3 * rp_, 35.0, -50.0, p_ * (0.5 * rp_) + q_ * (0.4 * rp_));
    case vStream: {
        // Behind, beside and a little above the departing debris, a few stream lengths away, looking back
        // past it towards the black hole: the stream large in the foreground, the hole behind it, receding
        // as the debris flies out to thousands of M before the bound half turns round. The debris is a
        // compact blob far from the hole (its length is about an eighth of its distance), so the camera
        // stays with it and backs off as it stretches. Anchored on the simulation's current debris position:
        // the particle box arrives a few frames late, and the debris moves several M per frame here.
        const glm::dvec3 c = in.pos;
        glm::dvec3 sDir = c - n_ * glm::dot(c, n_);
        sDir = glm::length(sDir) > 1e-9 ? glm::normalize(sDir) : -p_;
        const glm::dvec3 side = glm::normalize(glm::cross(n_, sDir));
        const double L = std::max(streamLen_, 1.0);
        const glm::dvec3 off = glm::normalize(sDir * 0.55 + n_ * 0.4 + side * 0.75);
        return {c + off * std::max(3.0 * L, 0.5 * rp_), c * 0.85};
    }
    case vHole: {
        // Over the hole, a little above the disk plane, drifting slowly round it; a slow push-in over the
        // first half minute of the disk stage.
        const double s = smooth01(diskSeconds_ / 30.0);
        return holePose((6.5 - 1.5 * s) * rp_, 22.0 - 8.0 * s, -110.0 + drift_, glm::dvec3(0.0));
    }
    }
    return {viewPos_, viewTarget_};
}

void TdeDirector::update(const Input& in) {
    if (stage_ == kIdle) return;
    if (!in.active) {
        stop();
        return;
    }
    rp_ = std::max(in.pericenterM, 2.5);
    const double dt = std::max(in.dt, 0.0);
    stageSeconds_ += dt;
    const double tOrb = std::pow(rp_, 1.5);
    const double r = std::max(glm::length(in.pos), 1e-6);
    if (tPeri_ < 0.0 && glm::dot(in.pos, in.vel) > 0.0) tPeri_ = in.timeM;
    const GasParticleStats* g = (in.gas && in.gas->valid) ? in.gas : nullptr;
    const double total = std::max(in.particles, 1);
    const double sincePeri = tPeri_ >= 0.0 ? in.timeM - tPeri_ : 0.0;
    const double wRet = wRetOf(rp_);

    // ---- stages
    switch (stage_) {
    case kApproach:
        if (in.swallowed) setStage(kSwallowed);
        else if (in.released) setStage(kTear);
        break;
    case kTear:
        if (tPeri_ >= 0.0 && sincePeri > 4.0 * tOrb && r > 3.0 * rp_) {
            setStage(kStream);
            accretedAtStream_ = in.accretedFraction;
            if (g && g->inPlay > 0) {
                streamCentre_ = 0.5 * (glm::dvec3(g->lo) + glm::dvec3(g->hi));
                streamLen_ = glm::length(glm::dvec3(g->hi) - glm::dvec3(g->lo));
            } else {
                streamCentre_ = in.pos;
                streamLen_ = 4.0 * rp_;
            }
        }
        break;
    case kStream: {
        // The return begins when the time-lapse has braked to the predicted first arrival, or, should the
        // prediction be wrong, when gas is unmistakably back at the hole.
        const bool farOut = sincePeri > 20.0 * tOrb;
        const bool braked = tFirst_ > 0.0 && in.timeM >= tFirst_ - 0.5 * wRet;
        const int threshold = std::max(50, static_cast<int>(0.006 * total));
        if (farOut && (braked || (g && g->nearHole >= threshold) || in.accretedFraction > accretedAtStream_ + 1e-2)) setStage(kReturn);
        break;
    }
    case kReturn:
        if (stageSeconds_ > 9.0) setStage(kDisk);
        break;
    default: break;
    }

    // ---- the predicted first return: the particles' Kepler orbits (beyond farRadiusM, where the Newtonian
    // phase is accurate) tell when the first returnFraction() of the star will be back at pericentre. The
    // estimate sharpens as the stream recedes; it is frozen shortly before the return, when the first gas
    // drops back inside farRadiusM and would push it later.
    if (stage_ == kStream && g && g->returnInM > 0.0 && sincePeri > 20.0 * tOrb &&
        (tFirst_ < 0.0 || in.timeM < tFirst_ - 0.15 * (tFirst_ - tPeri_)))
        tFirst_ = g->timeM + g->returnInM;

    // ---- pace of the event clock
    double target = pace_;
    switch (stage_) {
    case kApproach: target = std::pow(std::max(r, rp_), 1.5) / 8.0; break;
    case kSwallowed:
        // In coordinate time a falling star all but freezes at the horizon: keep it moving.
        target = std::pow(std::max(r, 6.0), 1.5) / 6.0;
        if (r < 4.0) target = std::max(target, 40.0);
        break;
    case kTear: target = std::pow(std::max(r, rp_), 1.5) / 2.4; break;   // about 8 s through pericentre
    case kStream: {
        target = 0.45 * std::max(sincePeri, tOrb);   // doubles every 1.5 s
        if (tFirst_ > 0.0) {
            if (in.timeM < tFirst_) {
                target = std::min(target, wRet + 0.9 * (tFirst_ - in.timeM));   // brakes over the last few seconds
                waitSeconds_ = 0.0;
            } else {
                waitSeconds_ += dt;   // predicted too early: speed up again until the gas is back
                target = wRet * std::exp(std::min(0.5 * waitSeconds_, 30.0));
            }
        }
        target = std::max(target, wRet);
        break;
    }
    // The debris comes back as a steady inflow lasting far longer than anyone watches (its energies are
    // spread widely), so after the first arrival in slow motion the clock speeds up to about 10x, and
    // slowly on while the disk forms.
    case kReturn: target = wRet * std::exp(std::min(stageSeconds_ / 3.0, std::log(10.0))); break;
    case kDisk: target = wRet * 10.0 * std::exp(std::min(0.04 * stageSeconds_, std::log(4.0))); break;
    default: break;
    }
    const double k = 1.0 - std::exp(-dt / 0.35);   // smooth, in log space
    pace_ = std::exp(std::log(std::max(pace_, 1e-9)) + (std::log(std::max(target, 1e-9)) - std::log(std::max(pace_, 1e-9))) * k);

    // ---- debris disk: forms as the gas comes back (and at least over half a minute of watching)
    if (stage_ == kReturn || stage_ == kDisk) {
        returnSeconds_ += dt;
        drift_ += 2.5 * dt;
        const double back = (g ? g->nearHole / total : 0.0) + std::max(in.accretedFraction - accretedAtStream_, 0.0);
        const double want = std::max(std::clamp(back / 0.03, 0.0, 1.0), std::clamp((returnSeconds_ - 3.0) / 25.0, 0.0, 1.0));
        disk_ += (want - disk_) * (1.0 - std::exp(-dt / 2.0));
    }
    if (stage_ == kDisk) diskSeconds_ += dt;

    // ---- riding along: a fixed distance from the star (its radius), the offset turning smoothly with it
    if (stage_ == kApproach || stage_ == kTear || stage_ == kSwallowed) {
        const double R = std::max(in.starRadiusM, 1e-6);
        const glm::dvec3 out = in.pos / r;
        const glm::dvec3 vh = glm::length(in.vel) > 0.0 ? glm::normalize(in.vel) : glm::dvec3(0.0);
        const glm::dvec3 want = glm::normalize(out + n_ * 0.45 - vh * 0.35);
        const double kr = rideInit_ ? 1.0 - std::exp(-dt / 0.6) : 1.0;
        rideDir_ = rideInit_ ? glm::normalize(slerpDir(rideDir_, want, kr, n_)) : want;
        rideDist_ = 14.0 * R;
        rideLook_ += ((stage_ == kTear ? 0.3 : 0.15) - rideLook_) * kr;
        rideInit_ = true;
    }

    // ---- framing of the stream (smoothed: the statistics arrive a few frames late)
    if (stage_ == kStream) {
        glm::dvec3 c = in.pos;
        double len = 4.0 * rp_;
        if (g && g->inPlay > 0) {
            c = 0.5 * (glm::dvec3(g->lo) + glm::dvec3(g->hi));
            len = glm::length(glm::dvec3(g->hi) - glm::dvec3(g->lo));
        }
        const double ks = 1.0 - std::exp(-dt / 0.8);
        streamCentre_ += (c - streamCentre_) * ks;
        streamLen_ += (len - streamLen_) * ks;
        // A few seconds before the predicted return, cut back to the hole to watch it arrive; also once the
        // bound debris has spread into an arc thousands of M across (nothing left to frame out there).
        if (tFirst_ > 0.0 && !earlyReturnView_) {
            const double secondsLeft = in.timeM < tFirst_ ? std::log((wRet + 0.9 * (tFirst_ - in.timeM)) / wRet) / 0.9 : 0.0;
            if (secondsLeft < 4.0) earlyReturnView_ = true;
        }
        if (streamLen_ > 170.0 * rp_) earlyReturnView_ = true;
    }

    // ---- shots: a fixed vantage per stage, joined by cuts of 0.8 s through black (the view switches at
    // the black frame) rather than camera moves, so each change of view is unmistakable.
    View want = vRide;
    switch (stage_) {
    // The tearing stays up close: at pericentre the star is squeezed into a slightly elongated blob (a
    // twentieth of the shadow's size for a Sun at Sagittarius A*) and only stretches into a long stream
    // as the debris climbs away, so from the ride the lengthening stream flows past the camera with the
    // hole receding behind it; no wider vantage shows anything but a speck.
    case kTear: want = vRide; break;
    case kStream: want = earlyReturnView_ ? vHole : vStream; break;
    case kReturn:
    case kDisk: want = vHole; break;
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
    const Pose cur = viewPose(view_, in);
    viewPos_ = cur.pos;
    viewTarget_ = cur.target;
}

}  // namespace bhs
