// Tests of the star-disruption director and of the return-time estimate it paces the event with.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "GasParticles.h"
#include "TdeDirector.h"
#include "tde/TdeReturn.h"

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

// The simulation's force law and lapse (tde_common.glsl: bhAccel, specificEnergy, lapseOf).
glm::dvec3 accel(const glm::dvec3& x, double L2) {
    const double r2 = glm::dot(x, x), r = std::sqrt(r2);
    return x * (-(1.0 + 3.0 * L2 / r2) / (r2 * r));
}
double energy(const glm::dvec3& x, const glm::dvec3& v) {
    const double r = glm::length(x);
    const double vr = glm::dot(x, v) / r;
    const glm::dvec3 l = glm::cross(x, v);
    return std::sqrt((1.0 - 2.0 / r) * (1.0 + glm::dot(l, l) / (r * r)) + vr * vr);
}

// Coordinate time until the next pericentre, by integrating the orbit (RK4 in proper time).
double integratedReturn(glm::dvec3 x, glm::dvec3 v) {
    const glm::dvec3 l = glm::cross(x, v);
    const double L2 = glm::dot(l, l);
    const double E = energy(x, v);
    double t = 0.0;
    double vrPrev = glm::dot(x, v) / glm::length(x);
    bool left = vrPrev > 0.0;   // must move outwards once before a pericentre counts
    for (int i = 0; i < 20000000; ++i) {
        const double r = glm::length(x);
        const double dtau = 0.002 * std::pow(r, 1.5);
        const glm::dvec3 k1v = accel(x, L2), k1x = v;
        const glm::dvec3 k2v = accel(x + 0.5 * dtau * k1x, L2), k2x = v + 0.5 * dtau * k1v;
        const glm::dvec3 k3v = accel(x + 0.5 * dtau * k2x, L2), k3x = v + 0.5 * dtau * k2v;
        const glm::dvec3 k4v = accel(x + dtau * k3x, L2), k4x = v + dtau * k3v;
        x += (dtau / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
        v += (dtau / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
        const double rn = glm::length(x);
        const double lapse = (1.0 - 2.0 / rn) / E;
        t += dtau / lapse;
        const double vr = glm::dot(x, v) / rn;
        if (vr > 0.0) left = true;
        if (left && vrPrev < 0.0 && vr >= 0.0) return t - dtau / lapse * vr / (vr - vrPrev);
        vrPrev = vr;
    }
    return -1.0;
}

// A particle on a Newtonian ellipse (a, e) at true anomaly f, in the orbit's plane.
void keplerState(double a, double e, double f, glm::dvec3& x, glm::dvec3& v) {
    const double p = a * (1.0 - e * e);
    const double r = p / (1.0 + e * std::cos(f));
    const double h = std::sqrt(p);
    x = glm::dvec3(r * std::cos(f), 0.0, r * std::sin(f));
    const double vr = e * std::sin(f) / h, vt = h / r;
    v = vr * glm::normalize(x) + vt * glm::dvec3(-std::sin(f), 0.0, std::cos(f));
}

void testReturnTime() {
    // Debris of a Sun-vs-Sgr A* event: pericentre ~9 M, apocentre thousands of M. Far out (r > 12 r_p),
    // the Kepler estimate must agree with the integrated orbit to a few percent, incoming or outgoing.
    for (const double a : {800.0, 3000.0}) {
        const double e = 1.0 - 9.0 / a;
        for (const double f : {1.2, 2.0, 3.0, 3.6, 4.5, 5.6}) {
            glm::dvec3 x, v;
            keplerState(a, e, f, x, v);
            if (glm::length(x) < 110.0) continue;
            const double est = bhs::tde::timeToNextPericentre(x, v);
            const double ref = integratedReturn(x, v);
            CHECK(est > 0.0 && ref > 0.0);
            // The Kepler estimate skips the strong-field passage, where coordinate time stretches by a few
            // tens of M: a few percent only for gas that is already falling in from close by.
            const double err = std::abs(est - ref) / ref;
            const bool ok = err < 0.02 || std::abs(est - ref) < 80.0;
            if (!ok) std::printf("  a %g f %g: estimate %g integrated %g (%.2f%%)\n", a, f, est, ref, 100.0 * err);
            CHECK(ok);
        }
    }
    // Unbound gas never comes back.
    const glm::dvec3 x(500.0, 0.0, 0.0), v(0.08, 0.0, 0.06);
    CHECK(bhs::tde::timeToNextPericentre(x, v) < 0.0);
}

// A synthetic event on a parabolic orbit with the director in charge of the clock.
void testDirector() {
    const double rp = 8.9, rt = 2.0 * rp;
    const double L = std::sqrt(2.0 * rp);
    auto state = [&](double f, glm::dvec3& pos, glm::dvec3& vel) {
        const double r = 2.0 * rp / (1.0 + std::cos(f));
        pos = glm::dvec3(r * std::cos(f), 0.0, r * std::sin(f));
        const double vt = L / r, vr = std::sqrt(std::max(2.0 / r - vt * vt, 0.0)) * (f < 0.0 ? -1.0 : 1.0);
        vel = vr * glm::normalize(pos) + vt * glm::dvec3(-std::sin(f), 0.0, std::cos(f));
    };
    double f = -std::acos(2.0 * rp / 50.0 - 1.0);   // r = 50 M, falling in
    glm::dvec3 pos, vel;
    state(f, pos, vel);
    bhs::TdeDirector d;
    d.start(pos, vel);
    CHECK(d.active() && d.stage() == bhs::TdeDirector::kApproach && d.cameraEngaged());

    const double dt = 1.0 / 60.0;
    const double tFirst = 8.4e4;              // the gas comes back at this event time
    const double wRet = kTwoPi * std::pow(2.0 * rp, 1.5) / 8.0;
    double timeM = 0.0;
    bool released = false, gasBack = false;
    bhs::GasParticleStats gas;
    std::vector<int> stages{d.stage()};
    double maxPaceBeforeReturn = 0.0, paceAtReturn = -1.0, returnSeconds = -1.0, diskAt = -1.0;
    double secondsSinceDisk = 0.0;
    for (int frame = 0; frame < 60 * 200; ++frame) {
        const double elapsed = frame * dt;
        bhs::TdeDirector::Input in;
        in.dt = dt;
        in.timeM = timeM;
        in.active = true;
        in.intact = !released;
        in.released = released;
        in.pericenterM = rp;
        in.starRadiusM = 1e-3;
        in.pos = pos;
        in.vel = vel;
        in.particles = 262144;
        gas.valid = released;
        gas.inPlay = 262144;
        gas.lo = glm::vec3(pos) - glm::vec3(0.1f * glm::length(pos) + 1.0f);
        gas.hi = glm::vec3(pos) + glm::vec3(0.1f * glm::length(pos) + 1.0f);
        gas.timeM = timeM;
        gas.returnInM = released && timeM > 2000.0 ? std::max(tFirst - timeM, 1.0) : -1.0;
        gas.nearHole = gasBack ? 5000 : 0;
        in.accretedFraction = gasBack ? 0.01 : 0.0;
        in.gas = &gas;
        d.update(in);
        if (d.stage() != stages.back()) stages.push_back(d.stage());
        glm::dvec3 vp, vt;
        d.view(vp, vt);
        CHECK(std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.z) && std::isfinite(vt.x));
        CHECK(glm::length(vp) > 2.5);
        if (d.stage() == bhs::TdeDirector::kStream && timeM < tFirst) maxPaceBeforeReturn = std::max(maxPaceBeforeReturn, d.pace());
        if (d.stage() == bhs::TdeDirector::kReturn && paceAtReturn < 0.0) {
            paceAtReturn = d.pace();
            returnSeconds = elapsed;
        }
        if (d.stage() == bhs::TdeDirector::kDisk) {
            if (diskAt < 0.0) diskAt = elapsed;
            secondsSinceDisk = elapsed - diskAt;
            if (secondsSinceDisk > 40.0 && d.diskPresence() > 0.9) break;
        }
        // The event clock and the star: the clock runs at the director's pace; the star follows its
        // parabola until far out, where the debris is simply "far away" until it returns.
        timeM += d.pace() * dt;
        const double r = glm::length(pos);
        if (r < 3000.0 || f < 0.0) {
            f += L / (r * r) * d.pace() * dt;
            f = std::min(f, 3.1);
            state(f, pos, vel);
        }
        if (!released && r < rt) released = true;
        if (released && timeM >= tFirst) gasBack = true;
    }
    const std::vector<int> want{bhs::TdeDirector::kApproach, bhs::TdeDirector::kTear, bhs::TdeDirector::kStream,
                                bhs::TdeDirector::kReturn, bhs::TdeDirector::kDisk};
    CHECK(stages == want);
    if (stages != want) {
        std::printf("  stages:");
        for (int s : stages) std::printf(" %d", s);
        std::printf("\n");
    }
    // The time-lapse brakes before the gas returns: the return stage opens near wRet (the pace is smoothed
    // over 0.35 s, so it lags the brake by a factor of about two) and plays on from there.
    CHECK(paceAtReturn > 0.0 && paceAtReturn < 3.0 * wRet);
    CHECK(maxPaceBeforeReturn > 10.0 * wRet);   // but the wait was a real time-lapse
    CHECK(returnSeconds > 0.0 && returnSeconds < 60.0);   // the whole wait fits in a minute
    CHECK(diskAt > 0.0 && d.diskPresence() > 0.9);
    std::printf("  return after %.1f s at pace %.1f M/s (wRet %.1f), disk at %.1f s, presence %.2f\n", returnSeconds, paceAtReturn,
                wRet, diskAt, d.diskPresence());
    // Titles and captions exist for every stage.
    CHECK(d.title()[0] != '\0' && d.caption()[0] != '\0');
    // Flying releases the camera; Watch takes it back; the event's end stops the director.
    d.releaseCamera();
    CHECK(!d.cameraEngaged());
    d.engageCamera();
    CHECK(d.cameraEngaged());
    bhs::TdeDirector::Input end;
    end.active = false;
    d.update(end);
    CHECK(!d.active() && !d.cameraEngaged());
}

}  // namespace

int main() {
    testReturnTime();
    testDirector();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tde_director_tests: all passed\n");
    return 0;
}
