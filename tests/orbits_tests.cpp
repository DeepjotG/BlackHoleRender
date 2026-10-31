// Checks for the Keplerian orbit helpers used for the solar system.
#include <cmath>
#include <cstdio>

#include "Orbits.h"

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

int main() {
    std::printf("Kepler's equation\n");
    double worst = 0.0;
    for (double e : {0.0, 0.0167, 0.2, 0.6, 0.95})
        for (double M = -3.0; M <= 3.0; M += 0.37) {
            const double E = orbits::eccentricAnomaly(M, e);
            worst = std::max(worst, std::abs(E - e * std::sin(E) - std::remainder(M, 2.0 * orbits::kPi)));
        }
    CHECK(worst < 1e-12, "residual of E - e sin E = M below 1e-12 (worst %.2e)", worst);

    std::printf("Earth-Moon barycentre (JPL table 1, 1800-2050)\n");
    universe::OrbitElements earth{1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
                                  0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0};
    const glm::dvec3 p = orbits::position(earth, 0.0);
    // JPL Horizons, Earth heliocentric ecliptic J2000 at 2000-01-01 12:00 TT: (-0.1771, 0.9672, 0.0000) AU.
    CHECK(std::abs(p.x + 0.1771) < 0.003 && std::abs(p.y - 0.9672) < 0.003 && std::abs(p.z) < 1e-3,
          "Earth at J2000 = (%.4f, %.4f, %.5f) AU, expected (-0.1771, 0.9672, 0)", p.x, p.y, p.z);
    const double r = glm::length(p);
    CHECK(r > 0.983 && r < 0.984, "Earth near perihelion in early January: %.4f AU", r);
    CHECK(std::abs(orbits::periodYears(earth) - 1.0) < 0.001, "Earth's period %.5f years", orbits::periodYears(earth));
    const glm::dvec3 half = orbits::position(earth, 0.5 / 100.0);
    CHECK(glm::dot(glm::normalize(p), glm::normalize(half)) < -0.99, "half a year later Earth is on the other side of the Sun");

    std::printf("Jupiter\n");
    universe::OrbitElements jup{5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
                                -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106};
    CHECK(std::abs(orbits::periodYears(jup) - 11.862) < 0.01, "Jupiter's period %.3f years", orbits::periodYears(jup));
    bool bounded = true;
    for (double T = -1.0; T <= 1.0; T += 0.013) {
        const double rr = glm::length(orbits::position(jup, T));
        bounded = bounded && rr > 4.94 && rr < 5.47;
    }
    CHECK(bounded, "Jupiter stays between perihelion and aphelion (4.95-5.46 AU)");
    const glm::dvec3 n = orbits::pole(jup, 0.0);
    CHECK(std::abs(std::acos(n.z) * 180.0 / orbits::kPi - 1.30439695) < 1e-9, "Jupiter's orbit pole is tilted 1.304 degrees");
    CHECK(std::abs(glm::dot(orbits::pole(jup, 0.3), glm::normalize(orbits::position(jup, 0.3)))) < 1e-12,
          "positions lie in the orbit plane of the same date");

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
