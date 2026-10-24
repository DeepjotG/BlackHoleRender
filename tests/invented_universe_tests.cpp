// Checks for the invented universe (src/universe/InventedUniverse.cpp).
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Units.h"
#include "universe/Universe.h"

using namespace bhs;
using namespace bhs::universe;

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
    constexpr double kpc = 1e3 * units::kParsec, mpc = 1e6 * units::kParsec;
    std::printf("Determinism and speed\n");
    UniverseContent a, b, c;
    const auto t0 = std::chrono::steady_clock::now();
    buildInventedUniverse(a, 2024u);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    buildInventedUniverse(b, 2024u);
    buildInventedUniverse(c, 2025u);
    bool same = a.galaxies.size() == b.galaxies.size() && a.stars.size() == b.stars.size() && a.names == b.names &&
                a.blackHolePos == b.blackHolePos;
    for (size_t i = 0; same && i < a.galaxies.size(); ++i) same = a.galaxies[i].model.center == b.galaxies[i].model.center;
    CHECK(same, "the same seed gives the same universe");
    CHECK(a.blackHolePos != c.blackHolePos && a.names != c.names, "another seed gives another universe");
    CHECK(ms < 300.0, "built in %.0f ms (< 300)", ms);
    CHECK(a.mode == Mode::Invented && a.blackHoleName == "Gargantua" && a.suggestedMassSolar == 0.0 && a.bodies.empty(),
          "mode, black hole name, no suggested mass, no planets");

    for (uint32_t seed : {2024u, 1u, 77u, 123456u}) {
        UniverseContent u;
        buildInventedUniverse(u, seed);
        std::printf("Seed %u\n", seed);
        const GalaxyModel& h = u.host;
        const glm::dvec3 rel = u.blackHolePos - h.center;
        const double y = glm::dot(rel, h.normal), R = glm::length(rel - y * h.normal);
        CHECK(R > 4.0 * kpc && R < 7.0 * kpc && std::abs(y) < 0.1 * kpc, "black hole in the disk at R = %.2f kpc, y = %.0f pc", R / kpc,
              y / units::kParsec);
        const double tilt = std::acos(std::clamp(glm::dot(glm::normalize(u.blackHoleSpinAxis), h.normal), -1.0, 1.0)) * 180.0 / 3.14159265358979;
        CHECK(tilt >= 34.9 && tilt <= 70.1, "spin axis tilted %.1f degrees from the galaxy's axis", tilt);
        CHECK(h.type == GalaxyType::Spiral || h.type == GalaxyType::BarredSpiral, "the host is a spiral (type %d)", static_cast<int>(h.type));
        CHECK(u.galaxies.size() >= 20000 && u.galaxies.size() <= 40000, "%zu galaxies", u.galaxies.size());
        int near50 = 0, named = 0;
        bool radii = true;
        for (const StarRecord& s : u.stars) {
            if (glm::length(s.pos - u.blackHolePos) < 50.0 * units::kParsec) ++near50;
            if (s.name >= 0 && s.name < static_cast<int32_t>(u.names.size())) ++named;
            radii = radii && s.radiusSolar > 0.005f && s.radiusSolar < 2000.0f && s.teff > 2500.0f && s.teff < 40000.0f;
        }
        CHECK(near50 >= 8 && named == static_cast<int>(u.stars.size()), "%d named notable stars within 50 pc", near50);
        CHECK(radii, "star radii and temperatures are physical");
        int satellites = 0, neighbours = 0, inside = 0;
        bool finite = true;
        for (const GalaxyRecord& g : u.galaxies) {
            const double d = glm::length(g.model.center - h.center);
            if (d > 40.0 * kpc && d < 320.0 * kpc) ++satellites;
            if (d > 0.6 * mpc && d < 1.6 * mpc && g.model.luminositySolar > 3e10) ++neighbours;
            if (d < 40.0 * kpc) ++inside;
            finite = finite && std::isfinite(g.absMag) && g.model.boundingRadiusPc() > 10.0 && g.model.boundingRadiusPc() < 3e5 &&
                     std::abs(glm::length(g.model.normal) - 1.0) < 1e-6 && std::abs(glm::dot(g.model.normal, g.model.major)) < 1e-6;
        }
        CHECK(satellites >= 2 && satellites <= 5, "%d satellite dwarfs at 50-300 kpc", satellites);
        CHECK(neighbours >= 1, "a large neighbour spiral at 0.7-1.5 Mpc");
        CHECK(inside == 0, "no galaxy overlaps the host");
        CHECK(finite, "galaxy parameters are finite with orthonormal axes");
        bool places = false;
        for (const NamedPlace& p : u.places) places = places || p.name.find("Cluster") != std::string::npos;
        CHECK(places && u.places.size() >= 2, "named places: the galactic centre and the nearest rich cluster");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
