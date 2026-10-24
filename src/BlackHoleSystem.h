#pragma once
// The black hole's physical scale (mass slider), the simulation clock and the planets that orbit
// it. Planets move on circular Schwarzschild geodesics: angular velocity d(phi)/dt = r^-1.5 in
// coordinate time, which is exact for circular orbits; orbits inside 6 M are unstable and orbits
// inside 3 M impossible.
#include <glm/glm.hpp>

#include <string>
#include <vector>

#include "universe/Universe.h"

namespace bhs {

struct Planet {
    std::string name;
    double orbitRadiusM = 10.0;     // M
    double inclinationDeg = 0.0;    // tilt of the orbit plane from the disk plane
    double nodeDeg = 0.0;           // longitude of the ascending node around +Y
    double phase = 0.0;             // orbital phase, radians (advanced by the clock)
    double radiusKm = 6371.0;
    double massEarth = 1.0;
    universe::PlanetLook look = universe::PlanetLook::Rocky;
    glm::vec3 tint{1.0f};
    float atmosphere = 0.0f;        // rim glow strength, 0 = airless
    uint32_t seed = 1;
    bool retrograde = false;

    // Derived each update (renderer frame, M and M per M).
    glm::dvec3 pos{0.0};
    glm::dvec3 vel{0.0};
    glm::dvec3 orbitNormal{0.0, 1.0, 0.0};
};

class BlackHoleSystem {
public:
    BlackHoleSystem();

    // The Interstellar-inspired trio: an ocean world just outside the innermost stable orbit, an
    // ice world and a desert world farther out.
    void resetPlanets();
    // Advances the orbits by dtM (coordinate time in M) and recomputes positions.
    void update(double dtM);
    void refresh();                 // recompute positions without advancing time

    std::vector<Planet> planets;

    // Coordinate time elapsed far away per unit proper time on the object (>= 1).
    static double timeDilationCircularOrbit(double rM);   // 1 / sqrt(1 - 3/r); infinite at r <= 3
    static double timeDilationStatic(double rM);          // 1 / sqrt(1 - 2/r) for a hovering observer
    static double orbitalPeriodM(double rM);              // 2 pi r^1.5, coordinate time
    static double orbitalSpeedLocal(double rM);           // speed measured by a static observer, in c
    // Distance (M) inside which the black hole's tides would pull a planet apart (rigid-body Roche limit).
    static double rocheLimitM(const Planet& p, double bhMassSolar);
    static double planetRadiusM(const Planet& p, double bhMassSolar);
};

}  // namespace bhs
