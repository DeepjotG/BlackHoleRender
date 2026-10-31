// Checks for the invented universe (src/universe/InventedUniverse.cpp): determinism and speed,
// black hole placement (on an arm, outside the bar, near the midplane) and tilt, the local group,
// counts, the named places (host centre, neighbour, nearest rich cluster), names, finite and sane
// galaxy parameters over a sweep of seeds, and the totals of the galaxies it makes.
//   g++ -std=c++20 -O2 -Isrc -I<glm> tests/invented_universe_tests.cpp src/universe/InventedUniverse.cpp
//       src/universe/GalaxyModel.cpp src/Units.cpp -o invented_universe_tests
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "Units.h"
#include "universe/GalaxyModel_internal.h"
#include "universe/Universe.h"

using namespace bhs;
using namespace bhs::universe;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                              \
    do {                                              \
        ++checks;                                     \
        char msg_[320];                               \
        std::snprintf(msg_, sizeof msg_, __VA_ARGS__); \
        if (cond) std::printf("  ok    %s\n", msg_);  \
        else { ++failures; std::printf("  FAIL  %s  (%s:%d)\n", msg_, __FILE__, __LINE__); } \
    } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kpc = 1e3 * units::kParsec, mpc = 1e6 * units::kParsec;

const NamedPlace* findPlace(const UniverseContent& u, const char* part) {
    for (const NamedPlace& p : u.places)
        if (p.name.find(part) != std::string::npos) return &p;
    return nullptr;
}

// Smooth-model totals (stars, light) on a log-spaced cylindrical grid out to 3 bounding radii.
std::pair<double, double> totals(const GalaxyModel& in) {
    GalaxyModel m = in;
    m.center = glm::dvec3(0.0);
    const detail::GalaxyEval g(m);
    const double rMax = 3.0 * g.boundRadiusPc() / 1000.0, rMin = 1e-5 * rMax, ys = 0.002;
    const double lnR = std::log(rMax / rMin), ay = std::asinh(rMax / ys);
    const int nR = 160, nT = 24, nY = 160;
    double stars = 0.0, light = 0.0;
    for (int i = 0; i < nR; ++i) {
        const double R = rMin * std::exp((i + 0.5) / nR * lnR), dR = R * lnR / nR;
        for (int k = 0; k < nY; ++k) {
            const double v = -1.0 + 2.0 * (k + 0.5) / nY;
            const double y = ys * std::sinh(v * ay), dy = ys * std::cosh(v * ay) * ay * 2.0 / nY;
            for (int j = 0; j < nT; ++j) {
                const double th = 2.0 * kPi * (j + 0.5) / nT;
                detail::GalaxySample s;
                g.sampleLocal(R * std::cos(th), y, R * std::sin(th), 1e3, detail::GalaxyEval::kStars | detail::GalaxyEval::kLight, s);
                const double dV = R * dR * dy * (2.0 * kPi / nT) * 1e9;
                stars += s.stars * dV;
                const glm::dvec3 l = s.starLight + s.nebula;
                light += (0.2126 * l.x + 0.7152 * l.y + 0.0722 * l.z) * dV;
            }
        }
    }
    return {stars / (m.stellarMassSolar / 0.4), light / m.luminositySolar};
}
}  // namespace

int main() {
    std::printf("Determinism and speed\n");
    UniverseContent a, b, c;
    const auto t0 = std::chrono::steady_clock::now();
    buildInventedUniverse(a, 2024u);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    buildInventedUniverse(b, 2024u);
    buildInventedUniverse(c, 2025u);
    bool same = a.galaxies.size() == b.galaxies.size() && a.stars.size() == b.stars.size() && a.names == b.names &&
                a.blackHolePos == b.blackHolePos && a.places.size() == b.places.size();
    for (size_t i = 0; same && i < a.galaxies.size(); ++i) same = a.galaxies[i].model.center == b.galaxies[i].model.center;
    for (size_t i = 0; same && i < a.places.size(); ++i) same = a.places[i].name == b.places[i].name && a.places[i].pos == b.places[i].pos;
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
        const double tilt = std::acos(std::clamp(glm::dot(glm::normalize(u.blackHoleSpinAxis), h.normal), -1.0, 1.0)) * 180.0 / kPi;
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
        const GalaxyRecord* neighbour = nullptr;
        for (const GalaxyRecord& g : u.galaxies) {
            const double d = glm::length(g.model.center - h.center);
            if (d > 40.0 * kpc && d < 320.0 * kpc) ++satellites;
            if (d > 0.6 * mpc && d < 1.6 * mpc && g.model.luminositySolar > 3e10) {
                ++neighbours;
                neighbour = &g;
            }
            if (d < 40.0 * kpc) ++inside;
            finite = finite && std::isfinite(g.absMag) && g.model.boundingRadiusPc() > 10.0 && g.model.boundingRadiusPc() < 3e5 &&
                     std::abs(glm::length(g.model.normal) - 1.0) < 1e-6 && std::abs(glm::dot(g.model.normal, g.model.major)) < 1e-6;
        }
        CHECK(satellites >= 2 && satellites <= 5, "%d satellite dwarfs at 50-300 kpc", satellites);
        CHECK(neighbours == 1, "a large neighbour spiral at 0.7-1.5 Mpc");
        CHECK(inside == 0, "no galaxy overlaps the host");
        CHECK(finite, "galaxy parameters are finite with orthonormal axes");
        // Named places: the host's centre, the neighbour and the nearest rich cluster.
        const NamedPlace* centre = findPlace(u, "Galactic centre");
        const NamedPlace* nb = findPlace(u, "(neighbour)");
        const NamedPlace* cluster = findPlace(u, "Cluster");
        CHECK(u.places.size() == 3 && centre && nb && cluster, "three named places: %s / %s / %s", centre ? centre->name.c_str() : "-",
              nb ? nb->name.c_str() : "-", cluster ? cluster->name.c_str() : "-");
        if (centre && nb && cluster && neighbour) {
            const std::string nbName = neighbour->name >= 0 ? u.names[static_cast<size_t>(neighbour->name)] : "";
            CHECK(centre->pos == h.center && centre->radiusMeters > 0.0 && centre->name.size() > std::string("Galactic centre ()").size(),
                  "the centre place is at the host's centre and names the host");
            CHECK(nb->pos == neighbour->model.center && nb->name.find(nbName) == 0 &&
                      std::abs(nb->radiusMeters - 0.5 * neighbour->model.boundingRadiusPc() * units::kParsec) < 1.0,
                  "the neighbour place is the neighbour galaxy (%.2f Mpc away)", glm::length(nb->pos - h.center) / mpc);
            const double dCl = glm::length(cluster->pos - u.blackHolePos);
            CHECK(dCl > 14.0 * mpc && dCl < 26.0 * mpc && cluster->radiusMeters > 0.1 * mpc, "the cluster place is %.1f Mpc away", dCl / mpc);
        }
    }

    std::printf("Sweep over 80 seeds\n");
    {
        int badR = 0, inBar = 0, badTilt = 0, badCounts = 0, badPlaces = 0, dupNames = 0, nonFinite = 0, richNearer = 0;
        double worstMs = 0.0;
        for (uint32_t i = 0; i < 80; ++i) {
            const uint32_t seed = 1000003u * i + 17u;
            UniverseContent u;
            const auto t = std::chrono::steady_clock::now();
            buildInventedUniverse(u, seed);
            worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            const GalaxyModel& h = u.host;
            const glm::dvec3 rel = u.blackHolePos - h.center;
            const double y = glm::dot(rel, h.normal), R = glm::length(rel - y * h.normal);
            badR += !(R > 4.19 * kpc && R < 6.81 * kpc && std::abs(y) < 0.1 * kpc);
            // Outside the bar, where the arms and the young disk begin.
            if (h.type == GalaxyType::BarredSpiral) inBar += !(R > 1.14 * h.barLengthPc * units::kParsec);
            const double tilt = std::acos(std::clamp(glm::dot(glm::normalize(u.blackHoleSpinAxis), h.normal), -1.0, 1.0)) * 180.0 / kPi;
            badTilt += !(tilt >= 34.9 && tilt <= 70.1);
            badCounts += !(u.galaxies.size() >= 20000 && u.galaxies.size() <= 40000);
            badPlaces += !(u.places.size() == 3 && findPlace(u, "Galactic centre") && findPlace(u, "(neighbour)") && findPlace(u, "Cluster"));
            std::set<std::string> names(u.names.begin(), u.names.end());
            dupNames += names.size() != u.names.size();
            for (const GalaxyRecord& g : u.galaxies)
                nonFinite += !(std::isfinite(g.model.center.x) && std::isfinite(g.absMag) && std::isfinite(g.model.boundingRadiusPc()));
            // The named cluster is the nearest rich one: no bright elliptical (> 4 L*) nearer than it,
            // outside the named cluster itself (within 3 Mpc of its centre), has 60 galaxies within 2 Mpc.
            const NamedPlace* cl = findPlace(u, "Cluster");
            if (cl) {
                const double dCl = glm::length(cl->pos);
                for (const GalaxyRecord& g : u.galaxies) {
                    if (g.model.type != GalaxyType::Elliptical || g.model.luminositySolar < 1e11) continue;
                    if (glm::length(g.model.center) >= dCl || glm::length(g.model.center - cl->pos) < 3.0 * mpc) continue;
                    int around = 0;
                    for (const GalaxyRecord& o : u.galaxies)
                        around += glm::length(o.model.center - g.model.center) < 2.0 * mpc && glm::length(o.model.center - cl->pos) >= 3.0 * mpc;
                    if (around >= 60) {
                        ++richNearer;
                        break;
                    }
                }
            }
        }
        CHECK(badR == 0 && badTilt == 0, "black hole at R = 4.2-6.8 kpc near the midplane, tilt 35-70 degrees (80 seeds)");
        CHECK(inBar == 0, "in barred hosts the black hole lies beyond the bar (%d inside)", inBar);
        CHECK(badCounts == 0 && badPlaces == 0, "20k-40k galaxies and the three named places for every seed");
        CHECK(dupNames == 0 && nonFinite == 0, "unique names, finite galaxies");
        CHECK(richNearer == 0, "no rich group lies nearer than the named cluster (%d seeds)", richNearer);
        CHECK(worstMs < 300.0, "slowest build %.0f ms", worstMs);
    }

    std::printf("Totals of the galaxies the universe makes (smooth model, out to 3 bounding radii)\n");
    {
        UniverseContent u;
        buildInventedUniverse(u, 2024u);
        std::vector<std::pair<std::string, GalaxyModel>> gals = {{"host", u.host}};
        size_t bcg = 0, len = 0, irr = 0, sat = 0;
        double lb = 0.0;
        for (size_t i = 0; i < u.galaxies.size(); ++i) {
            const GalaxyModel& m = u.galaxies[i].model;
            const double d = glm::length(m.center) / kpc;
            if (d < 400.0 && sat < 2) gals.push_back({"satellite " + std::to_string(sat++), m});
            if (d > 600.0 && d < 1600.0 && m.luminositySolar > 3e10) gals.push_back({"neighbour", m});
            if (m.type == GalaxyType::Elliptical && m.luminositySolar > lb) lb = m.luminositySolar, bcg = i;
            if (m.type == GalaxyType::Lenticular && !len) len = i;
            if (m.type == GalaxyType::Irregular && d > 2000.0 && !irr) irr = i;
        }
        gals.push_back({"brightest elliptical", u.galaxies[bcg].model});
        gals.push_back({"lenticular", u.galaxies[len].model});
        gals.push_back({"web irregular", u.galaxies[irr].model});
        for (const auto& [name, m] : gals) {
            const auto [stars, light] = totals(m);
            CHECK(std::abs(stars - 1.0) < 0.06 && std::abs(light - 1.0) < 0.2, "%s (type %d): stars %.3f of M/0.4, light %.3f of L", name.c_str(),
                  static_cast<int>(m.type), stars, light);
        }
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
