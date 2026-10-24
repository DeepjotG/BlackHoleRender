// Placeholder invented universe (used only while src/universe/InventedUniverse.cpp is missing):
// a default spiral host galaxy, a few thousand explicit stars around the black hole and a few
// hundred distant galaxies, enough to exercise the 3D sky pipeline.
#include <cmath>
#include <random>

#include "Units.h"
#include "universe/Universe.h"

namespace bhs::universe {

void buildInventedUniverse(UniverseContent& out, uint32_t seed) {
    out = UniverseContent{};
    out.mode = Mode::Invented;
    std::mt19937 rng(seed * 2654435761u + 3u);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::normal_distribution<double> N(0.0, 1.0);
    const double pc = units::kParsec;
    out.host = GalaxyModel{};
    out.host.seed = seed;
    out.blackHolePos = glm::dvec3(5500.0 * pc, 15.0 * pc, 0.0);
    const double tilt = glm::radians(50.0);
    out.blackHoleSpinAxis = glm::normalize(glm::dvec3(std::sin(tilt), std::cos(tilt), 0.3));
    out.blackHoleName = "Gargantua";
    out.names.push_back("Gargantua");
    for (int i = 0; i < 6000; ++i) {
        StarRecord s;
        const glm::dvec3 dir = glm::normalize(glm::dvec3(N(rng), N(rng) * 0.35, N(rng)));
        const double r = 300.0 * std::cbrt(U(rng)) * pc;
        s.pos = out.blackHolePos + dir * r;
        const double u = U(rng);
        s.absMag = static_cast<float>(-6.0 + 20.0 * std::pow(u, 0.6));
        s.teff = static_cast<float>(3000.0 + 25000.0 * std::pow(U(rng), 3.0));
        s.radiusSolar = static_cast<float>(std::pow(10.0, (4.83 - s.absMag) / 5.0) * std::pow(5772.0 / s.teff, 2.0));
        out.stars.push_back(s);
    }
    for (int i = 0; i < 600; ++i) {
        GalaxyRecord g;
        const glm::dvec3 dir = glm::normalize(glm::dvec3(N(rng), N(rng), N(rng)));
        g.model.center = dir * ((0.5 + 60.0 * U(rng)) * 1e6 * pc);
        g.model.normal = glm::normalize(glm::dvec3(N(rng), N(rng), N(rng)));
        glm::dvec3 m = glm::cross(g.model.normal, glm::dvec3(0.3, 1.0, 0.1));
        g.model.major = glm::normalize(m);
        const double scale = 0.3 + 1.2 * U(rng);
        g.model.diskScaleLengthPc *= scale;
        g.model.diskCutoffPc *= scale;
        g.model.luminositySolar *= scale * scale;
        g.model.type = U(rng) < 0.3 ? GalaxyType::Elliptical : GalaxyType::Spiral;
        g.model.seed = static_cast<uint32_t>(i + 1);
        out.galaxies.push_back(g);
    }
    out.places.push_back({"Host galaxy centre", glm::dvec3(0.0), 3000.0 * pc});
}

}  // namespace bhs::universe
