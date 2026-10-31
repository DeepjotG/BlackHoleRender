// Checks for the black hole system: units, circular-orbit physics and planet bookkeeping.
#include <cmath>
#include <cstdio>
#include <string>

#include "BlackHoleSystem.h"
#include "Units.h"

using namespace bhs;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                              \
    do {                                              \
        ++checks;                                     \
        char msg_[256];                               \
        std::snprintf(msg_, sizeof msg_, __VA_ARGS__); \
        if (cond) std::printf("  ok    %s\n", msg_);  \
        else { ++failures; std::printf("  FAIL  %s  (%s:%d)\n", msg_, __FILE__, __LINE__); } \
    } while (0)

static bool near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)); }

int main() {
    std::printf("geometric units\n");
    const double mGarg = units::metersPerM(1e8);
    CHECK(near(mGarg, 1.4766e11, 1e-3), "1 M of a 1e8 solar-mass hole = %.4e m (expect 1.4766e11)", mGarg);
    CHECK(near(units::secondsPerM(1e8), 492.5, 2e-3), "1 M of time = %.1f s (expect 492.5)", units::secondsPerM(1e8));
    CHECK(near(units::metersPerM(1.0), 1476.6, 1e-3), "1 solar mass: 1 M = %.1f m", units::metersPerM(1.0));

    std::printf("circular orbits in Schwarzschild\n");
    CHECK(near(BlackHoleSystem::timeDilationCircularOrbit(6.0), std::sqrt(2.0), 1e-12), "ISCO time dilation = sqrt(2)");
    CHECK(std::isinf(BlackHoleSystem::timeDilationCircularOrbit(3.0)), "no circular orbit at the photon sphere");
    CHECK(near(BlackHoleSystem::timeDilationStatic(4.0), std::sqrt(2.0), 1e-12), "hovering at 4 M: dilation sqrt(2)");
    CHECK(near(BlackHoleSystem::orbitalSpeedLocal(6.0), 0.5, 1e-12), "orbital speed at the ISCO = c/2");
    CHECK(near(BlackHoleSystem::orbitalPeriodM(10.0), 2.0 * M_PI * std::pow(10.0, 1.5), 1e-12), "period = 2 pi r^1.5");

    std::printf("planet orbits\n");
    BlackHoleSystem sys;
    CHECK(sys.planets.size() == 3, "three preset planets");
    bool radiiOk = true, tangentOk = true, progradeOk = true;
    for (const Planet& p : sys.planets) {
        radiiOk = radiiOk && near(glm::length(p.pos), p.orbitRadiusM, 1e-12);
        tangentOk = tangentOk && std::abs(glm::dot(glm::normalize(p.pos), glm::normalize(p.vel))) < 1e-12;
        progradeOk = progradeOk && glm::dot(glm::cross(p.pos, p.vel), glm::dvec3(0, 1, 0)) > 0.0;
    }
    CHECK(radiiOk, "positions lie on the orbit radius");
    CHECK(tangentOk, "velocities are tangential");
    CHECK(progradeOk, "orbits share the disk's sense of rotation (angular momentum along +Y)");
    {
        Planet& p = sys.planets[0];
        const double phase0 = p.phase;
        const double period = BlackHoleSystem::orbitalPeriodM(p.orbitRadiusM);
        sys.update(period * 0.25);
        const double moved = std::fmod(p.phase - phase0 + 4.0 * M_PI, 2.0 * M_PI);
        CHECK(near(moved, M_PI / 2.0, 1e-9), "a quarter period advances the phase by pi/2 (got %.6f)", moved);
        const double speed = glm::length(p.vel);
        CHECK(near(speed, std::sqrt(1.0 / p.orbitRadiusM), 1e-9), "coordinate speed sqrt(M/r)");
    }

    std::printf("tides\n");
    Planet earth;
    earth.radiusKm = units::kEarthRadius / 1e3;
    earth.massEarth = 1.0;
    const double rocheGarg = BlackHoleSystem::rocheLimitM(earth, 1e8);
    CHECK(rocheGarg > 1.5 && rocheGarg < 2.0, "Earth's Roche limit around Gargantua = %.3f M (inside the horizon region)", rocheGarg);
    const double rocheSgr = BlackHoleSystem::rocheLimitM(earth, 4.297e6);
    CHECK(rocheSgr > 10.0 && rocheSgr < 20.0, "around Sgr A* = %.2f M, so close planets are torn apart", rocheSgr);

    std::printf("formatting\n");
    CHECK(units::formatDistance(units::kAU * 4.56) == "4.56 AU", "4.56 AU formats as '%s'", units::formatDistance(units::kAU * 4.56).c_str());
    CHECK(units::formatDuration(3600.0 * 3.2) == "3.20 h", "3.2 hours formats as '%s'", units::formatDuration(3600.0 * 3.2).c_str());
    CHECK(units::formatSpeed(0.45 * units::kC) == "0.45 c", "0.45 c formats as '%s'", units::formatSpeed(0.45 * units::kC).c_str());

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
