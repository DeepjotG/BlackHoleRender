// Checks for the real universe (src/universe/RealUniverse.cpp) against the generated catalogues in
// assets/real (tools/fetch_real_catalogs.py): frames, catalogue stars and galaxies, de-reddening with
// the Milky Way model's dust, the Milky Way model, the solar system, placements, completeness fields,
// load time and robustness.
//
//   g++ -std=c++20 -O2 -pthread -Isrc -I<glm> tests/real_universe_tests.cpp src/universe/RealUniverse.cpp
//       src/universe/GalaxyModel.cpp src/Units.cpp -o real_universe_tests && ./real_universe_tests [assetDir]
// assetDir defaults to <source dir>/../assets (derived from __FILE__).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "Units.h"
#include "universe/RealUniverse_internal.h"
#include "universe/Universe.h"

using namespace bhs;
using namespace bhs::universe;
using glm::dvec3;

// The 1 s load budget applies to optimised builds; sanitizer instrumentation slows the de-reddening
// (1.9 million dust evaluations) about tenfold.
#if defined(__SANITIZE_ADDRESS__)
constexpr double kLoadBudgetSeconds = 10.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr double kLoadBudgetSeconds = 10.0;
#else
constexpr double kLoadBudgetSeconds = 1.0;
#endif
#else
constexpr double kLoadBudgetSeconds = 1.0;
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        const bool ok_ = (cond); /* before formatting: cond may update args */ \
        char msg_[512];                                                        \
        std::snprintf(msg_, sizeof msg_, __VA_ARGS__);                         \
        if (ok_) {                                                             \
            std::printf("  ok    %s\n", msg_);                                 \
        } else {                                                               \
            ++g_failures;                                                      \
            std::printf("  FAIL  %s   (%s:%d)\n", msg_, __FILE__, __LINE__);   \
        }                                                                      \
    } while (0)

constexpr double kDeg = std::numbers::pi / 180.0;

static double angleDeg(const dvec3& a, const dvec3& b) {
    const double c = glm::dot(glm::normalize(a), glm::normalize(b));
    const double s = glm::length(glm::cross(glm::normalize(a), glm::normalize(b)));
    return std::atan2(s, c) / kDeg;
}

static double wrap180(double d) {
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return d;
}

// Galactocentric direction -> ICRS RA/Dec and IAU l/b.
static void dirToRaDec(const dvec3& galDir, double& ra, double& dec) {
    real::icrsToRaDec(glm::transpose(real::icrsToGalactocentric()) * galDir, ra, dec);
}
static void dirToLB(const dvec3& galDir, double& l, double& b) {
    real::galacticLB(glm::transpose(real::icrsToGalactocentric()) * galDir, l, b);
}

static int findStar(const UniverseContent& c, const std::string& name) {
    for (size_t i = 0; i < c.stars.size(); ++i) {
        if (c.stars[i].name >= 0 && c.names[c.stars[i].name] == name) return static_cast<int>(i);
    }
    return -1;
}
static const GalaxyRecord* findGalaxy(const UniverseContent& c, const std::string& name) {
    for (const GalaxyRecord& g : c.galaxies) {
        if (g.name >= 0 && c.names[g.name] == name) return &g;
    }
    return nullptr;
}
static const NamedPlace* findPlace(const UniverseContent& c, const std::string& name) {
    for (const NamedPlace& p : c.places) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

static double appMagFrom(const StarRecord& s, const dvec3& from) {
    const double dPc = std::max(glm::length(s.pos - from) / units::kParsec, 1e-6);
    return s.absMag + 5.0 * std::log10(dPc / 10.0);
}

// The raw catalogue files (record i of stars.bin is star i of the content: nothing is skipped in the
// real data, which testContent checks).
static real::StarFile g_starFile;
static real::GalaxyFile g_galaxyFile;

// Catalogue V of star i: the file's absMag (V - 5 log d/10, extinction included) plus the modulus.
static double catalogueV(const UniverseContent& c, size_t i) {
    const dvec3 sun = real::sunGalactocentricPc() * units::kParsec;
    return g_starFile.records[i].absMag + 5.0 * std::log10(std::max(glm::length(c.stars[i].pos - sun) / units::kParsec, 1e-6) / 10.0);
}

// Reference column with a fine midpoint rule over the whole segment (no slab clipping).
static double referenceColumn(const GalaxyModel& g, const dvec3& a, const dvec3& b, double stepPc) {
    const double len = glm::length(b - a) / units::kParsec;
    const int n = std::max(1, static_cast<int>(std::ceil(len / stepPc)));
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += g.dustExtinction(a + (b - a) * ((i + 0.5) / n));
    return s * len / n;
}

// Sense in which the model's arms wind on our sky (north up, east left): +1 counter-clockwise outward,
// -1 clockwise, from the position angles of points along an arm crest seen from the Sun.
static int windingOnSky(const GalaxyModel& m) {
    const dvec3 sun = real::sunGalactocentricPc() * units::kParsec;
    const glm::dmat3 toIcrs = glm::transpose(real::icrsToGalactocentric());
    const dvec3 losC = glm::normalize(toIcrs * (m.center - sun));
    const dvec3 east = glm::normalize(glm::cross(dvec3(0, 0, 1), losC)), north = glm::cross(losC, east);
    const dvec3 e2 = glm::cross(m.normal, m.major);
    double total = 0.0, prev = 0.0;
    for (int k = 0; k <= 200; ++k) {
        const double rPc = m.diskScaleLengthPc * (0.5 + 2.5 * k / 200.0);
        // Crest of arm 0: psi = 0 (GalaxyModel.h).
        const double th = std::log(rPc / m.armRefRadiusPc) / std::tan(m.armPitchDeg * kDeg) + m.armPhaseDeg * kDeg;
        const dvec3 p = m.center + rPc * units::kParsec * (std::cos(th) * m.major + std::sin(th) * e2);
        const dvec3 v = glm::normalize(toIcrs * (p - sun)) - losC;
        const double pa = std::atan2(glm::dot(v, east), glm::dot(v, north));
        if (k > 0) total += std::remainder(pa - prev, 2.0 * std::numbers::pi);
        prev = pa;
    }
    return total > 0.0 ? +1 : -1;
}

// ------------------------------------------------------------------------------------------------

static void testFrames() {
    std::printf("frames\n");
    const glm::dmat3& m = real::icrsToGalactocentric();
    const glm::dmat3 mtm = glm::transpose(m) * m;
    double err = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) err = std::max(err, std::fabs(mtm[i][j] - (i == j ? 1.0 : 0.0)));
    CHECK(err < 1e-12 && std::fabs(glm::determinant(m) - 1.0) < 1e-12, "ICRS -> galactocentric is a proper rotation (|M^T M - I| = %.1e)", err);

    const dvec3 sun = real::sunGalactocentricPc();
    CHECK(std::fabs(glm::length(sun) - 8178.0) < 1e-6 && std::fabs(sun.y - 20.8) < 1e-9 && sun.x < 0.0 && sun.z == 0.0,
          "Sun at (%.2f, %.2f, %.2f) pc: 8178 pc from Sgr A*, 20.8 pc above the plane", sun.x, sun.y, sun.z);
    const dvec3 sunM = real::icrsPcToGalactocentricMeters(dvec3(0.0));
    // 2.5e20 m at double precision: agreement to ~1e-16 relative is exact.
    CHECK(glm::length(sunM - sun * units::kParsec) < 1e-15 * 8178.0 * units::kParsec * 4.0,
          "ICRS origin maps to the Sun (|diff| = %.2e m of %.2e m)", glm::length(sunM - sun * units::kParsec), glm::length(sunM));

    // Sagittarius A* seen from the Sun.
    double ra, dec, l, b;
    dirToRaDec(-sun, ra, dec);
    CHECK(std::fabs(ra - 266.4168) < 1e-3 && std::fabs(dec - -29.0078) < 1e-3, "Sgr A* direction from the Sun: RA %.5f Dec %.5f (266.4168, -29.0078)", ra, dec);
    dirToLB(-sun, l, b);
    CHECK(std::fabs(wrap180(l - 359.944)) < 0.01 && std::fabs(b - -0.046) < 0.01, "Sgr A* at galactic l %.4f b %.4f (359.944, -0.046)", l, b);
    const dvec3 sgr = real::icrsPcToGalactocentricMeters(8178.0 * real::icrsUnit(real::kSgrARaDeg, real::kSgrADecDeg));
    CHECK(glm::length(sgr) < 1e6, "Sgr A* at 8178 pc maps to the origin (|pos| = %.2e m)", glm::length(sgr));

    // North galactic pole (IAU: RA 192.85948, Dec +27.12825) is +Y to within the solar tilt.
    const dvec3 ngp = m * real::icrsUnit(192.85948, 27.12825);
    CHECK(angleDeg(ngp, dvec3(0, 1, 0)) < 0.25, "north galactic pole is %.3f deg from +Y", angleDeg(ngp, dvec3(0, 1, 0)));
    double lp, bp;
    real::galacticLB(real::icrsUnit(192.85948, 27.12825), lp, bp);
    CHECK(bp > 89.999, "IAU matrix puts the NGP at b = %.5f", bp);
    // l = 0, b = 0 is close to +X; l = 90 (direction of Galactic rotation) close to -Z (the Sun orbits towards -Z).
    const dvec3 icrsL90 = glm::transpose(real::icrsToGalacticIau()) * dvec3(0, 1, 0);
    const dvec3 icrsL0 = glm::transpose(real::icrsToGalacticIau()) * dvec3(1, 0, 0);
    CHECK(angleDeg(m * icrsL0, dvec3(1, 0, 0)) < 0.2 && angleDeg(m * icrsL90, dvec3(0, 0, -1)) < 0.2,
          "l=0 -> +X (%.3f deg), l=90 -> -Z (%.3f deg); +Z = X x Y", angleDeg(m * icrsL0, dvec3(1, 0, 0)), angleDeg(m * icrsL90, dvec3(0, 0, -1)));
    // Round trip RA/Dec.
    real::icrsToRaDec(real::icrsUnit(123.456, -54.321), ra, dec);
    CHECK(std::fabs(ra - 123.456) < 1e-9 && std::fabs(dec + 54.321) < 1e-9, "RA/Dec round trip");
    // Ecliptic north pole: RA 270, Dec 90 - 23.43928.
    real::icrsToRaDec(real::eclipticToIcrs() * dvec3(0, 0, 1), ra, dec);
    CHECK(std::fabs(ra - 270.0) < 1e-9 && std::fabs(dec - (90.0 - 23.43928)) < 1e-9, "ecliptic pole at RA %.4f Dec %.5f", ra, dec);
}

static void testOrbits() {
    std::printf("Kepler orbits\n");
    const OrbitElements earth{1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
                              0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0};
    const dvec3 p0 = real::keplerEclipticAU(earth, 0.0);
    CHECK(std::fabs(glm::length(p0) - 0.98329) < 2e-4, "Earth at J2000 is %.5f AU from the Sun (near perihelion, 0.98329)", glm::length(p0));
    const double lon = std::atan2(p0.y, p0.x) / kDeg;
    CHECK(std::fabs(wrap180(lon - 100.4)) < 0.3, "Earth heliocentric ecliptic longitude %.2f deg (100.4)", lon);
    const dvec3 pHalf = real::keplerEclipticAU(earth, 0.5 / 100.0);
    CHECK(std::fabs(glm::length(pHalf) - 1.0167) < 1e-3, "Earth half a year later is %.4f AU (near aphelion)", glm::length(pHalf));
    const dvec3 pYear = real::keplerEclipticAU(earth, 1.0 / 100.0);
    CHECK(angleDeg(pYear, p0) < 0.05, "Earth returns after one Julian year (%.3f deg)", angleDeg(pYear, p0));
    // Kepler's equation at high eccentricity converges.
    const OrbitElements ecc{1.0, 0.95, 0, 10.0, 0, 0, 0, 0, 0, 0, 0, 0};
    const dvec3 pe = real::keplerEclipticAU(ecc, 0.0);
    CHECK(std::isfinite(pe.x) && glm::length(pe) > 0.05 && glm::length(pe) < 1.95, "e = 0.95 orbit position finite (r = %.4f AU)", glm::length(pe));
}

static void testContent(const UniverseContent& c, double loadSeconds) {
    std::printf("catalogue content\n");
    CHECK(c.mode == Mode::Real, "mode is Real");
    CHECK(c.stars.size() > 115000 && c.stars.size() < 125000, "%zu stars (HYG v4.4 has 119,614 entries)", c.stars.size());
    CHECK(c.galaxies.size() > 44000 && c.galaxies.size() < 47000, "%zu galaxies (2MRS 44,599 + Local Volume)", c.galaxies.size());
    CHECK(c.bodies.size() == 9, "%zu solar system bodies", c.bodies.size());
    CHECK(c.places.size() >= 18, "%zu named places", c.places.size());
    CHECK(loadSeconds < kLoadBudgetSeconds, "loaded in %.0f ms (< %.0f s)", loadSeconds * 1e3, kLoadBudgetSeconds);
    CHECK(real::lastLoadReport().starsSkipped == 0 && real::lastLoadReport().galaxiesSkipped == 0, "no invalid records");

    // Names: indices valid, galaxy names unique.
    bool idxOk = true;
    for (const StarRecord& s : c.stars) idxOk &= s.name >= -1 && s.name < static_cast<int32_t>(c.names.size());
    for (const GalaxyRecord& g : c.galaxies) idxOk &= g.name >= -1 && g.name < static_cast<int32_t>(c.names.size());
    CHECK(idxOk, "all name indices are valid (%zu names)", c.names.size());
    std::set<std::string> gnames;
    size_t dup = 0, named = 0;
    for (const GalaxyRecord& g : c.galaxies) {
        if (g.name < 0) continue;
        ++named;
        dup += !gnames.insert(c.names[g.name]).second;
    }
    CHECK(dup == 0 && named > 10000, "%zu named galaxies, %zu duplicate names", named, dup);

    // Stars.
    const dvec3 sunPos = real::sunGalactocentricPc() * units::kParsec;
    const int sun = findStar(c, "Sun");
    CHECK(sun == 0 && c.stars[0].pos == sunPos && c.stars[0].absMag == 4.83f && c.stars[0].teff == 5772.0f,
          "star 0 is the Sun at the galactocentric solar position");
    bool finiteOk = true;
    for (const StarRecord& s : c.stars) {
        finiteOk &= std::isfinite(s.pos.x) && std::isfinite(s.pos.y) && std::isfinite(s.pos.z) && s.teff >= 2000.0f &&
                    s.teff <= 60000.0f && s.radiusSolar > 0.0f && s.radiusSolar <= 1800.0f;
    }
    CHECK(finiteOk, "every star has a finite position, 2000 K <= Teff <= 60000 K, 0 < R <= 1800 R_sun");
    CHECK(g_starFile.records.size() == c.stars.size() && g_galaxyFile.records.size() == c.galaxies.size(),
          "content stars and galaxies correspond one to one to the file records");
    if (g_starFile.records.size() != c.stars.size()) return;
    size_t nakedEye = 0;
    for (size_t i = 1; i < c.stars.size(); ++i) nakedEye += catalogueV(c, i) < 6.5;
    CHECK(nakedEye > 8000 && nakedEye < 9500, "%zu stars brighter than V = 6.5 seen from the Sun", nakedEye);

    // De-reddening: absMag = max(file absMag - A_V(Sun -> star), -10) with the host model's dust, so
    // seen from the Sun through that dust every star not at the limit has its catalogue V.
    {
        size_t checked = 0, wrong = 0, capped = 0, reproduced = 0, notReproduced = 0;
        double worst = 0.0;
        for (size_t i = 1; i < c.stars.size(); ++i) {
            const bool naked = catalogueV(c, i) < 6.5;
            if (!naked && i % 20 != 0) continue;
            const double a = real::hostDustColumnMag(c.host, sunPos, c.stars[i].pos);
            const double want = std::max(g_starFile.records[i].absMag - a, static_cast<double>(real::kBrightestStarAbsMag));
            ++checked;
            wrong += std::fabs(c.stars[i].absMag - want) > 1e-4;
            const double vSeen = appMagFrom(c.stars[i], sunPos) + a;
            if (g_starFile.records[i].absMag - a < real::kBrightestStarAbsMag) {
                ++capped;
            } else {
                worst = std::max(worst, std::fabs(vSeen - catalogueV(c, i)));
                (std::fabs(vSeen - catalogueV(c, i)) < 1e-3 ? reproduced : notReproduced) += 1;
            }
        }
        CHECK(wrong == 0, "absMag = file absMag - host dust column for %zu stars (naked-eye and every 20th)", checked);
        CHECK(notReproduced == 0 && reproduced > 13000, "seen from the Sun through the model's dust, %zu stars have their catalogue V "
              "(worst %.1e mag); %zu at the luminosity limit", reproduced, worst, capped);
        float brightest = 0.0f;
        for (const StarRecord& s : c.stars) brightest = std::min(brightest, s.absMag);
        CHECK(brightest >= real::kBrightestStarAbsMag && real::lastLoadReport().starsAtBrightestLimit < 120,
              "no star intrinsically brighter than M_V = %.0f (%zu capped by model dust far thicker than the real dust)",
              real::kBrightestStarAbsMag, real::lastLoadReport().starsAtBrightestLimit);
        CHECK(real::lastLoadReport().medianExtinction > 0.005 && real::lastLoadReport().medianExtinction < 0.2,
              "median A_V from the Sun to the catalogue stars %.3f mag (max %.1f)", real::lastLoadReport().medianExtinction,
              real::lastLoadReport().maxExtinction);
    }

    const auto checkStar = [&](const char* name, double raDeg, double decDeg, double dMin, double dMax) {
        const int i = findStar(c, name);
        if (i < 0) {
            CHECK(false, "%s present", name);
            return;
        }
        const dvec3 v = c.stars[i].pos - sunPos;
        const double dPc = glm::length(v) / units::kParsec;
        double ra, dec;
        dirToRaDec(v, ra, dec);
        const double sep = angleDeg(real::icrsUnit(ra, dec), real::icrsUnit(raDeg, decDeg));
        CHECK(dPc > dMin && dPc < dMax && sep < 0.01, "%s at %.3f pc, RA %.4f Dec %.4f (%.4f deg off), Teff %.0f K, R %.2f R_sun",
              name, dPc, ra, dec, sep, c.stars[i].teff, c.stars[i].radiusSolar);
    };
    checkStar("Sirius", 101.2872, -16.7161, 2.60, 2.68);          // 6h45m08.9s -16d42m58s, 2.64 pc
    checkStar("Betelgeuse", 88.7929, 7.4071, 140.0, 230.0);       // 5h55m10.3s +7d24m25s, 150-220 pc
    checkStar("Proxima Centauri", 217.4290, -62.6795, 1.29, 1.31);
    checkStar("Polaris", 37.9545, 89.2641, 120.0, 140.0);
    checkStar("Vega", 279.2347, 38.7837, 7.5, 7.9);
    const int sirius = findStar(c, "Sirius");
    if (sirius >= 0) {
        const StarRecord& s = c.stars[sirius];
        CHECK(std::fabs(s.teff - 9940.0f) < 400.0f && std::fabs(s.radiusSolar - 1.71f) < 0.2f && std::fabs(s.absMag - 1.45f) < 0.05f,
              "Sirius A: Teff %.0f K (9940), R %.2f R_sun (1.71), M_V %.2f (1.45)", s.teff, s.radiusSolar, s.absMag);
    }
    const int betel = findStar(c, "Betelgeuse");
    if (betel >= 0) {
        CHECK(c.stars[betel].teff > 3300.0f && c.stars[betel].teff < 4200.0f && c.stars[betel].radiusSolar > 300.0f,
              "Betelgeuse is a cool supergiant (Teff %.0f K, R %.0f R_sun)", c.stars[betel].teff, c.stars[betel].radiusSolar);
    }
    // Stars without parallaxes got finite, bounded distances (HYG has 100 kpc there).
    double maxD = 0.0;
    for (const StarRecord& s : c.stars) maxD = std::max(maxD, glm::length(s.pos - sunPos) / units::kParsec);
    CHECK(maxD <= 6000.5, "farthest catalogue star %.0f pc from the Sun (no 100 kpc placeholders)", maxD);
    const int eta = findStar(c, "Eta Carinae");
    CHECK(eta >= 0 && std::fabs(glm::length(c.stars[eta].pos - sunPos) / units::kParsec - 2350.0) < 1.0, "Eta Carinae at 2350 pc");

    // Galaxies.
    const GalaxyRecord* m31 = findGalaxy(c, "Andromeda Galaxy (M31)");
    CHECK(m31 != nullptr, "Andromeda Galaxy (M31) present");
    if (m31) {
        const dvec3 v = m31->model.center - sunPos;
        const double dKpc = glm::length(v) / (1e3 * units::kParsec);
        double l, b;
        dirToLB(v, l, b);
        CHECK(std::fabs(dKpc - 765.0) < 765.0 * 0.02, "Andromeda at %.1f kpc (~765)", dKpc);
        CHECK(std::fabs(wrap180(l - 121.17)) < 0.03 && std::fabs(b - -21.57) < 0.03, "Andromeda towards l %.3f b %.3f (121.17, -21.57)", l, b);
        const double incl = angleDeg(m31->model.normal, -v);
        CHECK(std::fabs(std::min(incl, 180.0 - incl) - 78.0) < 1.0, "M31 disk inclined %.1f deg to the line of sight (77-78)", std::min(incl, 180.0 - incl));
        // Literature disk scale lengths of M31 are ~4.5-6 kpc (e.g. Courteau et al. 2011).
        CHECK(m31->model.type == GalaxyType::Spiral && m31->model.diskScaleLengthPc > 4000 && m31->model.diskScaleLengthPc < 7000 &&
              m31->absMag < -21.0f, "M31 is a spiral, scale length %.0f pc, M_V %.2f", m31->model.diskScaleLengthPc, m31->absMag);
        // M31's north-west side (PA 305) is the near side.
        const dvec3 losIcrs = glm::normalize(glm::transpose(real::icrsToGalactocentric()) * v);
        const dvec3 east = glm::normalize(glm::cross(dvec3(0, 0, 1), losIcrs)), north = glm::cross(losIcrs, east);
        const double pa = 305.0 * kDeg;
        const dvec3 nwSky = real::icrsToGalactocentric() * (std::cos(pa) * north + std::sin(pa) * east);
        // The in-disk direction that projects on the NW minor axis tilts towards the observer.
        const dvec3 n = m31->model.normal;
        const dvec3 losGal = glm::normalize(v);
        const dvec3 inDisk = glm::normalize(nwSky - glm::dot(nwSky, n) / glm::dot(losGal, n) * losGal);
        CHECK(glm::dot(inDisk, -losGal) > 0.0, "M31's north-west side is nearer to us");
    }
    struct Want { const char* name; double minKpc, maxKpc; };
    const Want wants[] = {{"Triangulum Galaxy (M33)", 780, 900},      {"Large Magellanic Cloud", 48, 51},
                          {"Small Magellanic Cloud", 60, 65},         {"Bode's Galaxy (M81)", 3400, 3900},
                          {"Cigar Galaxy (M82)", 3300, 3900},         {"Virgo A (M87)", 14500, 18500},
                          {"Centaurus A (NGC 5128)", 3500, 4000},     {"Sombrero Galaxy (M104)", 8500, 10500},
                          {"Whirlpool Galaxy (M51)", 7000, 9500},     {"Pinwheel Galaxy (M101)", 6500, 7500},
                          {"Sculptor Galaxy (NGC 253)", 3300, 4200},  {"Sagittarius Dwarf Spheroidal", 20, 30},
                          {"Fornax Dwarf", 130, 160},                 {"Barnard's Galaxy (NGC 6822)", 440, 520},
                          {"Southern Pinwheel Galaxy (M83)", 4000, 5500}, {"Hidden Galaxy (IC 342)", 3000, 3600}};
    for (const Want& w : wants) {
        const GalaxyRecord* g = findGalaxy(c, w.name);
        const double d = g ? glm::length(g->model.center - sunPos) / (1e3 * units::kParsec) : -1.0;
        CHECK(g && d > w.minKpc && d < w.maxKpc, "%s at %.0f kpc", w.name, d);
    }
    const GalaxyRecord* lmc = findGalaxy(c, "Large Magellanic Cloud");
    CHECK(lmc && lmc->model.type == GalaxyType::BarredSpiral && lmc->model.barLengthPc > 0.0, "the LMC is a barred Magellanic spiral");
    const GalaxyRecord* m87 = findGalaxy(c, "Virgo A (M87)");
    CHECK(m87 && m87->model.type == GalaxyType::Elliptical && m87->model.stellarMassSolar > 1e11, "M87 is a giant elliptical (%.2e M_sun)",
          m87 ? m87->model.stellarMassSolar : 0.0);

    // Every galaxy model is well formed.
    size_t bad = 0;
    double farMpc = 0.0;
    for (const GalaxyRecord& g : c.galaxies) {
        const GalaxyModel& m = g.model;
        const bool ok = std::fabs(glm::length(m.normal) - 1.0) < 1e-9 && std::fabs(glm::length(m.major) - 1.0) < 1e-9 &&
                        std::fabs(glm::dot(m.normal, m.major)) < 1e-9 && std::isfinite(m.center.x) &&
                        std::isfinite(m.center.y) && std::isfinite(m.center.z) && m.stellarMassSolar >= 100.0 &&
                        std::isfinite(m.stellarMassSolar) && m.luminositySolar > 0.0 && std::isfinite(m.luminositySolar) &&
                        m.diskScaleLengthPc > 0.0 && m.diskScaleHeightPc > 0.0 && m.diskCutoffPc > m.diskScaleLengthPc &&
                        m.bulgeRadiusPc > 0.0 && m.ellipticalRadiusPc > 0.0 && m.ellipticalAxisRatio > 0.0 &&
                        m.ellipticalAxisRatio <= 1.0 && m.armCount >= 1 && m.dustExtinctionPerKpc >= 0.0 &&
                        std::isfinite(m.dustExtinctionPerKpc) && m.dustScaleHeightPc > 0.0 &&
                        static_cast<int>(m.type) >= 0 && static_cast<int>(m.type) <= 4 && std::isfinite(g.absMag);
        if (!ok && bad < 5) {
            std::printf("        bad model %s: type %d M* %.3g L %.3g h %.3g hz %.3g cut %.3g bulge %.3g re %.3g q %.3g arms %d dust %.3g/%.3g\n",
                        g.name >= 0 ? c.names[g.name].c_str() : "(unnamed)", static_cast<int>(m.type), m.stellarMassSolar,
                        m.luminositySolar, m.diskScaleLengthPc, m.diskScaleHeightPc, m.diskCutoffPc, m.bulgeRadiusPc,
                        m.ellipticalRadiusPc, m.ellipticalAxisRatio, m.armCount, m.dustExtinctionPerKpc, m.dustScaleHeightPc);
        }
        bad += !ok;
        farMpc = std::max(farMpc, glm::length(m.center) / (1e6 * units::kParsec));
    }
    CHECK(bad == 0, "all galaxy models well formed (%zu bad); farthest at %.0f Mpc", bad, farMpc);
    std::set<uint32_t> seeds;
    for (const GalaxyRecord& g : c.galaxies) seeds.insert(g.model.seed);
    CHECK(seeds.size() > c.galaxies.size() * 99 / 100, "galaxy seeds are distinct (%zu of %zu)", seeds.size(), c.galaxies.size());

    // Completeness fields for procedural star suppression.
    CHECK(c.catalogCenter == sunPos && c.catalogRadiusPc >= 500.0 && c.catalogRadiusPc <= 3000.0 && c.catalogLimitAppMag == 7.5f,
          "catalogue completeness: V <= %.1f within %.0f pc of the Sun", c.catalogLimitAppMag, c.catalogRadiusPc);
    size_t brighterOutside = 0, brighter = 0;
    for (size_t i = 1; i < c.stars.size(); ++i) {
        if (catalogueV(c, i) > c.catalogLimitAppMag) continue;
        ++brighter;
        brighterOutside += glm::length(c.stars[i].pos - sunPos) / units::kParsec > c.catalogRadiusPc;
    }
    CHECK(brighterOutside <= brighter / 100 + 1, "%zu of %zu catalogue stars brighter than the limit lie outside the radius", brighterOutside, brighter);
}

static void testDustColumn(const UniverseContent& c) {
    std::printf("host dust column (de-reddening)\n");
    const dvec3 sun = real::sunGalactocentricPc() * units::kParsec;
    const GalaxyModel& mw = c.host;
    CHECK(real::hostDustColumnMag(mw, sun, sun) == 0.0, "zero-length segment has no extinction");
    // Observed A_V ~ 30-40; GalaxyModel thins the dust inside the bar, which gives ~14 mag. Either way
    // the Galactic Centre is invisible in V; the bounds also catch unit errors (mag/pc vs mag/kpc).
    const double toGc = real::hostDustColumnMag(mw, sun, dvec3(0.0));
    CHECK(toGc > 8.0 && toGc < 60.0, "A_V from the Sun to Sgr A* %.1f mag: opaque (observed 30-40)", toGc);
    const double back = real::hostDustColumnMag(mw, dvec3(0.0), sun);
    CHECK(std::fabs(back - toGc) < 1e-9 * toGc, "the column is symmetric (%.6f vs %.6f)", toGc, back);
    const double pc = units::kParsec;
    CHECK(real::hostDustColumnMag(mw, dvec3(-8000, 3000, 0) * pc, dvec3(-7000, 2500, 500) * pc) == 0.0,
          "segments above the dust layer are skipped");
    GalaxyModel clean = mw;
    clean.dustExtinctionPerKpc = 0.0;
    CHECK(real::hostDustColumnMag(clean, sun, dvec3(0.0)) == 0.0, "a dust-free host gives no extinction");
    // Beyond the host's bounding radius there is no dust: a 1 Mpc segment costs no more than the galaxy.
    const dvec3 rim = glm::normalize(dvec3(-1.0, 0.0, 0.3)) * (mw.boundingRadiusPc() * 0.999 * pc);
    const double toRim = real::hostDustColumnMag(mw, sun, rim);
    const auto tFar = std::chrono::steady_clock::now();
    const double toFar = real::hostDustColumnMag(mw, sun, sun + glm::normalize(rim - sun) * (1e6 * pc));
    const double farMs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tFar).count() * 1e3;
    CHECK(std::isfinite(toFar) && std::fabs(toFar - toRim) < 0.02 * toRim + 0.01 && farMs < 20.0,
          "a 1 Mpc sight line is clipped at the host's edge (%.3f vs %.3f mag to the edge, %.2f ms)", toFar, toRim, farMs);
    // Against a 4x finer midpoint rule over the whole (unclipped) segment, for stars with some dust.
    double sq = 0.0, worst = 0.0, sumRef = 0.0;
    size_t n = 0;
    for (size_t i = 1; i < c.stars.size() && n < 400; i += 97) {
        const double ref = referenceColumn(mw, sun, c.stars[i].pos, 5.0);
        if (ref < 0.05) continue;
        const double a = real::hostDustColumnMag(mw, sun, c.stars[i].pos);
        sq += (a - ref) * (a - ref);
        sumRef += ref;
        worst = std::max(worst, std::fabs(a - ref));
        ++n;
    }
    CHECK(n > 100 && std::sqrt(sq / n) < 0.03 && worst < 0.3,
          "%zu sight lines vs a 5 pc reference: rms %.4f mag, worst %.3f mag (mean column %.2f)", n, std::sqrt(sq / n), worst,
          sumRef / std::max<size_t>(n, 1));
}

static void testCatalogueFixes(const UniverseContent& c) {
    std::printf("catalogue fixes\n");
    const dvec3 sunPos = real::sunGalactocentricPc() * units::kParsec;
    const double pc = units::kParsec, kpc = 1e3 * units::kParsec;
    // Orion Nebula: the Trapezium stars are at the nebula (they had spectroscopic distances near 1 kpc).
    const NamedPlace* orion = findPlace(c, "Orion Nebula (M42)");
    for (const char* name : {"Theta1 Orionis A", "Theta1 Orionis C", "Theta1 Orionis D", "Theta2 Orionis"}) {
        const int i = findStar(c, name);
        const double d = orion && i >= 0 ? glm::length(c.stars[i].pos - orion->pos) / pc : -1.0;
        CHECK(i >= 0 && d >= 0.0 && d < 5.0, "%s is %.2f pc from the Orion Nebula place", name, d);
    }
    const int t1c = findStar(c, "Theta1 Orionis C");
    CHECK(t1c >= 0 && c.stars[t1c].teff > 35000.0f, "Theta1 Orionis C is the hot O7 star (Teff %.0f K)", t1c >= 0 ? c.stars[t1c].teff : 0.0f);
    const int mira = findStar(c, "Mira");
    CHECK(mira >= 0 && c.stars[mira].teff == 3000.0f && c.stars[mira].radiusSolar == 350.0f,
          "Mira: Teff %.0f K, R %.0f R_sun (interferometry: ~3000 K, 330-400 R_sun)", mira >= 0 ? c.stars[mira].teff : 0.0f,
          mira >= 0 ? c.stars[mira].radiusSolar : 0.0f);
    // No shells of stars at the limits of the spectroscopic distances (1,189 sat at exactly 400 pc).
    std::vector<double> est;
    for (size_t i = 1; i < c.stars.size(); ++i) {
        if (g_starFile.records[i].flags & real::kStarDistanceEstimated) est.push_back(glm::length(c.stars[i].pos - sunPos) / pc);
    }
    std::sort(est.begin(), est.end());
    size_t maxSame = 0;
    for (size_t i = 0, j = 0; i < est.size(); i = j) {
        for (j = i; j < est.size() && est[j] - est[i] < 0.1; ++j) {
        }
        maxSame = std::max(maxSame, j - i);
    }
    const auto near = [&](double d0) {
        return std::count_if(est.begin(), est.end(), [&](double d) { return std::fabs(d - d0) < 0.5; });
    };
    CHECK(est.size() > 10000 && maxSame < 20 && near(400.0) < 30 && near(6000.0) == 0 && est.back() <= 6000.5,
          "%zu estimated distances: at most %zu within 0.1 pc of each other, %td at 400 +- 0.5 pc, %td at 6000 pc",
          est.size(), maxSame, near(400.0), near(6000.0));

    // Galaxies behind the Milky Way's dust have extinction-corrected sizes (catalogue D25 equivalents).
    const auto diameterKpc = [&](const char* name) {
        for (size_t i = 0; i < c.galaxies.size(); ++i) {
            if (c.galaxies[i].name >= 0 && c.names[c.galaxies[i].name] == name) return static_cast<double>(g_galaxyFile.records[i].diameterKpc);
        }
        return -1.0;
    };
    CHECK(diameterKpc("Maffei 1") > 8.0 && diameterKpc("Maffei 2") > 10.0, "Maffei 1 and 2: D %.1f and %.1f kpc (2MASS: 10.7, 12.3)",
          diameterKpc("Maffei 1"), diameterKpc("Maffei 2"));
    CHECK(diameterKpc("Hidden Galaxy (IC 342)") > 28.0 && diameterKpc("Circinus Galaxy (ESO 97-G13)") > 15.0,
          "IC 342 D %.1f kpc, Circinus D %.1f kpc", diameterKpc("Hidden Galaxy (IC 342)"), diameterKpc("Circinus Galaxy (ESO 97-G13)"));
    const double m31d = diameterKpc("Andromeda Galaxy (M31)"), m81d = diameterKpc("Bode's Galaxy (M81)");
    CHECK(m31d > 35.0 && m31d < 50.0 && m81d > 22.0 && m81d < 35.0, "unobscured sizes unchanged: M31 %.1f kpc, M81 %.1f kpc", m31d, m81d);

    // Shape fields of disks: effective radius and axis ratio.
    size_t irr = 0, irrBad = 0, len = 0, lenBad = 0;
    for (size_t i = 0; i < c.galaxies.size(); ++i) {
        const GalaxyModel& m = c.galaxies[i].model;
        const real::GalaxyFileRecord& r = g_galaxyFile.records[i];
        const double q = std::clamp(static_cast<double>(r.axisRatio), 0.1, 1.0);
        if (m.type == GalaxyType::Irregular) {
            ++irr;
            const double h = m.diskScaleLengthPc;
            const double want = r.effRadiusKpc > 0.0f ? r.effRadiusKpc * 1000.0 : 1.678 * h;
            const bool scales = r.effRadiusKpc > 0.0f || h <= 30.0 || h >= 3.0e4 ||
                                std::fabs(m.ellipticalRadiusPc / (r.diameterKpc * 1000.0) - 1.678 / 5.6) < 1e-6;
            irrBad += std::fabs(m.ellipticalRadiusPc - want) > 1e-6 * want || !scales ||
                      std::fabs(m.ellipticalAxisRatio - std::min(q, 0.35)) > 1e-9;
        } else if (m.type == GalaxyType::Lenticular) {
            ++len;
            const double want = r.effRadiusKpc > 0.0f ? r.effRadiusKpc * 1000.0 : 1.678 * m.diskScaleLengthPc;
            lenBad += std::fabs(m.ellipticalAxisRatio - q) > 1e-9 || std::fabs(m.ellipticalRadiusPc - want) > 1e-6 * want;
        }
    }
    CHECK(irr > 1500 && irrBad == 0, "%zu irregulars: effective radius 1.678 h = 0.300 D25 (or measured), axis ratio min(b/a, 0.35)", irr);
    CHECK(len > 10000 && lenBad == 0, "%zu lenticulars: observed axis ratio (sprites) and disk half-light radius", len);
    const GalaxyRecord* m82 = findGalaxy(c, "Cigar Galaxy (M82)");
    CHECK(m82 && m82->model.type == GalaxyType::Irregular && m82->model.ellipticalAxisRatio <= 0.35 + 1e-9 &&
              m82->model.ellipticalRadiusPc > 2500.0 && m82->model.ellipticalRadiusPc < 5000.0,
          "M82: irregular, effective radius %.0f pc, flattening %.2f", m82 ? m82->model.ellipticalRadiusPc : 0.0,
          m82 ? m82->model.ellipticalAxisRatio : 0.0);
    const GalaxyRecord* n3115 = findGalaxy(c, "NGC 3115");
    CHECK(n3115 && n3115->model.type == GalaxyType::Lenticular && n3115->model.ellipticalAxisRatio < 0.5,
          "edge-on S0 NGC 3115 drawn with axis ratio %.2f", n3115 ? n3115->model.ellipticalAxisRatio : 0.0);

    // Geometry of famous systems.
    const GalaxyRecord* m51 = findGalaxy(c, "Whirlpool Galaxy (M51)");
    const GalaxyRecord* n5195 = findGalaxy(c, "NGC 5195");
    if (m51 && n5195) {
        const dvec3 v = m51->model.center - sunPos;
        const double incl = angleDeg(m51->model.normal, -v);
        CHECK(std::fabs(std::min(incl, 180.0 - incl) - 22.0) < 0.5, "M51 inclined %.1f deg (kinematic 22)", std::min(incl, 180.0 - incl));
        double ra, dec;
        dirToRaDec(v, ra, dec);
        const double off = angleDeg(real::icrsUnit(ra, dec), real::icrsUnit(202.46957, 47.19526)) * 3600.0;
        CHECK(off < 10.0, "M51 at its nucleus (%.1f arcsec from the 2MASS position; UNGC is 2.3' off)", off);
        CHECK(glm::length(n5195->model.center - m51->model.center) < 30.0 * kpc, "NGC 5195 is %.0f kpc from M51 (was 740)",
              glm::length(n5195->model.center - m51->model.center) / kpc);
    } else {
        CHECK(false, "M51 and NGC 5195 present");
    }
    const GalaxyRecord* m31 = findGalaxy(c, "Andromeda Galaxy (M31)");
    const GalaxyRecord* m32 = findGalaxy(c, "M32 (NGC 221)");
    const GalaxyRecord* m110 = findGalaxy(c, "M110 (NGC 205)");
    if (m31 && m32 && m110) {
        const double d32 = glm::length(m32->model.center - m31->model.center) / kpc;
        const double d110 = glm::length(m110->model.center - m31->model.center) / kpc;
        CHECK(d32 < 30.0 && d110 < 50.0, "M32 %.0f kpc and M110 %.0f kpc from M31 (McConnachie: 22 and 41)", d32, d110);
        CHECK(std::fabs(m31->model.stellarMassSolar - 1e11) < 1e9, "M31 stellar mass %.2e M_sun (literature ~1e11)", m31->model.stellarMassSolar);
    } else {
        CHECK(false, "M31, M32 and M110 present");
    }
    // Arm winding on our sky matches images for the galaxies whose orientation is fixed.
    for (const char* name : {"Andromeda Galaxy (M31)", "Whirlpool Galaxy (M51)", "Bode's Galaxy (M81)", "Pinwheel Galaxy (M101)",
                             "Southern Pinwheel Galaxy (M83)", "Phantom Galaxy (M74)"}) {
        const GalaxyRecord* g = findGalaxy(c, name);
        const int want = real::armWindingOverride(name);
        const int got = g ? windingOnSky(g->model) : 0;
        CHECK(g && want != 0 && got == want, "%s: arms wind %s outward on the sky, as in images", name,
              got > 0 ? "counter-clockwise" : "clockwise");
    }
}

static void testMilkyWay(const UniverseContent& c) {
    std::printf("Milky Way host\n");
    const GalaxyModel& mw = c.host;
    CHECK(mw.type == GalaxyType::BarredSpiral && mw.center == dvec3(0.0) && mw.normal == dvec3(0, 1, 0) &&
          std::fabs(glm::dot(mw.normal, mw.major)) < 1e-12, "barred spiral centred on Sgr A*, normal = north galactic pole");
    CHECK(std::fabs(mw.stellarMassSolar - 5e10) < 1e10 && std::fabs(mw.luminositySolar - 3e10) < 1e10 && mw.diskScaleLengthPc == 2600.0 &&
          mw.diskScaleHeightPc == 300.0, "M* %.1e, L_V %.1e, h_R %.0f pc, h_z %.0f pc", mw.stellarMassSolar, mw.luminositySolar,
          mw.diskScaleLengthPc, mw.diskScaleHeightPc);
    CHECK(mw.armCount == 4 && mw.armPitchDeg == 12.0 && mw.barLengthPc >= 4500.0 && mw.barLengthPc <= 5000.0,
          "%d arms, pitch %.0f deg, bar half-length %.0f pc", mw.armCount, mw.armPitchDeg, mw.barLengthPc);
    // Bar ends seen from the Sun: near end at positive longitude, 27 deg from the Sun-centre line.
    const dvec3 e1 = mw.major, e2 = glm::cross(mw.normal, mw.major);
    const double th = mw.barAngleDeg * kDeg;
    const dvec3 nearEnd = (std::cos(th) * e1 + std::sin(th) * e2) * mw.barLengthPc;
    const dvec3 sun = real::sunGalactocentricPc();
    double l1, b1, l2, b2;
    dirToLB(nearEnd - sun, l1, b1);
    dirToLB(-nearEnd - sun, l2, b2);
    CHECK(glm::length(nearEnd - sun) < glm::length(-nearEnd - sun) && l1 > 0.0 && l1 < 90.0 && l2 > 270.0,
          "near end of the bar at l = %.1f, far end at l = %.1f", l1, l2);
    const double barAngle = angleDeg(nearEnd, sun);
    CHECK(std::fabs(barAngle - 27.0) < 0.5, "bar at %.1f deg to the Sun-centre line", barAngle);
    // Arm phase convention of GalaxyModel.h: an arm crosses the Sun's azimuth at 7.05 kpc (Sagittarius-Carina)
    // and the Sun lies between arms.
    const auto armPhase = [&](double rPc, double theta) {
        return mw.armCount * (theta - std::log(rPc / mw.armRefRadiusPc) / std::tan(mw.armPitchDeg * kDeg) - mw.armPhaseDeg * kDeg);
    };
    const double sunTheta = std::atan2(glm::dot(sun, e2), glm::dot(sun, e1));
    CHECK(std::cos(armPhase(7050.0, sunTheta)) > 0.999 && std::cos(armPhase(9840.0, sunTheta)) > 0.99 &&
          std::cos(armPhase(glm::length(glm::dvec2(sun.x, sun.z)), sunTheta)) < -0.5,
          "arms cross the Sun's azimuth at 7.05 and 9.84 kpc; the Sun is between arms (cos psi = %.2f)",
          std::cos(armPhase(glm::length(glm::dvec2(sun.x, sun.z)), sunTheta)));
}

static void testSolarSystem(const UniverseContent& c) {
    std::printf("solar system\n");
    const char* names[] = {"Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune", "Pluto"};
    const PlanetLook looks[] = {PlanetLook::Rocky, PlanetLook::Cloudy, PlanetLook::Earthlike, PlanetLook::Desert,
                                PlanetLook::GasGiantWarm, PlanetLook::GasGiantWarm, PlanetLook::GasGiantCold,
                                PlanetLook::GasGiantCold, PlanetLook::Ice};
    bool ok = c.bodies.size() == 9;
    for (size_t i = 0; ok && i < 9; ++i) {
        ok &= c.bodies[i].name == names[i] && c.bodies[i].look == looks[i] && c.bodies[i].hostStar == 0 &&
              c.bodies[i].radiusKm > 1000.0 && c.bodies[i].massEarth > 0.0;
    }
    CHECK(ok, "Mercury..Pluto orbit the Sun with the requested looks");
    if (c.bodies.size() != 9) return;
    const OrbitingBody& earth = c.bodies[2];
    CHECK(earth.radiusKm == 6371.0 && earth.massEarth == 1.0, "Earth radius %.1f km, mass %.2f", earth.radiusKm, earth.massEarth);
    const dvec3 sunPos = c.stars[earth.hostStar].pos;
    const dvec3 earthPos = sunPos + earth.orbitFrame * real::keplerEclipticAU(earth.elements, 0.0) * units::kAU;
    const double rAU = glm::length(earthPos - sunPos) / units::kAU;
    CHECK(std::fabs(rAU - 1.0) < 0.02, "Earth %.5f AU from the Sun at J2000", rAU);
    // The Sun seen from Earth at J2000 (2000-01-01 12h TT): RA 281.3, Dec -23.0.
    double ra, dec;
    dirToRaDec(sunPos - earthPos, ra, dec);
    CHECK(std::fabs(ra - 281.3) < 0.3 && std::fabs(dec - -23.03) < 0.2, "the Sun seen from Earth at J2000: RA %.2f Dec %.2f (281.3, -23.0)", ra, dec);
    // orbitFrame takes the ecliptic pole to RA 270, Dec 66.56.
    dirToRaDec(earth.orbitFrame * dvec3(0, 0, 1), ra, dec);
    CHECK(std::fabs(ra - 270.0) < 1e-6 && std::fabs(dec - 66.56072) < 1e-4, "orbitFrame maps the ecliptic pole to RA %.4f Dec %.4f", ra, dec);
    const OrbitingBody& jupiter = c.bodies[4];
    const double pj = real::orbitalPeriodYears(jupiter.elements);
    CHECK(std::fabs(pj - 11.86) < 0.01 && std::fabs(pj - std::pow(jupiter.elements.aAU, 1.5)) < 0.02,
          "Jupiter's period %.3f years (11.86; Kepler III gives %.3f)", pj, std::pow(jupiter.elements.aAU, 1.5));
    const OrbitingBody& saturn = c.bodies[5];
    CHECK(saturn.rings && saturn.ringInnerKm == 74500.0 && saturn.ringOuterKm == 140220.0 && saturn.ringInnerKm > saturn.radiusKm,
          "Saturn's rings from %.0f to %.0f km", saturn.ringInnerKm, saturn.ringOuterKm);
    int ringed = 0;
    for (const OrbitingBody& b : c.bodies) ringed += b.rings;
    CHECK(ringed == 1, "only Saturn has rings");
    const OrbitingBody& pluto = c.bodies[8];
    CHECK(std::fabs(real::orbitalPeriodYears(pluto.elements) - 247.9) < 1.0 && std::fabs(pluto.elements.iDeg - 17.14) < 0.01,
          "Pluto: period %.1f years, inclination %.2f deg", real::orbitalPeriodYears(pluto.elements), pluto.elements.iDeg);
    // All planets lie in a thin ecliptic disk around the Sun at J2000.
    const dvec3 eclipticPole = earth.orbitFrame * dvec3(0, 0, 1);
    double maxIncl = 0.0;
    for (const OrbitingBody& b : c.bodies) {
        const dvec3 p = b.orbitFrame * real::keplerEclipticAU(b.elements, 0.0);
        const double r = glm::length(p);
        CHECK(r > 0.3 && r < 50.0, "%s at %.2f AU", b.name.c_str(), r);
        if (b.name != "Pluto") maxIncl = std::max(maxIncl, std::fabs(90.0 - angleDeg(p, eclipticPole)));
    }
    CHECK(maxIncl < 7.1, "planets within %.2f deg of the ecliptic", maxIncl);
}

static void testPlaces(const UniverseContent& c) {
    std::printf("named places\n");
    const char* required[] = {"Galactic Centre (Sagittarius A*)", "Solar System", "Orion Nebula (M42)", "Pleiades (M45)", "Hyades",
                              "Local Group", "Virgo Cluster", "Coma Cluster", "Great Attractor (Norma Cluster)", "Fornax Cluster",
                              "Perseus Cluster", "Shapley Supercluster", "Omega Centauri", "Crab Nebula (M1)"};
    for (const char* r : required) {
        const NamedPlace* p = findPlace(c, r);
        CHECK(p && std::isfinite(p->pos.x) && p->radiusMeters > 0.0, "%s", r);
    }
    const dvec3 sunPos = real::sunGalactocentricPc() * units::kParsec;
    const NamedPlace* gc = findPlace(c, "Galactic Centre (Sagittarius A*)");
    CHECK(gc && glm::length(gc->pos) == 0.0, "Galactic Centre at the origin");
    const NamedPlace* pl = findPlace(c, "Pleiades (M45)");
    const int alcyone = findStar(c, "Alcyone");
    CHECK(pl && alcyone >= 0 && glm::length(pl->pos - c.stars[alcyone].pos) / units::kParsec < 5.0,
          "the Pleiades place is among its catalogue stars (%.1f pc from Alcyone)",
          pl && alcyone >= 0 ? glm::length(pl->pos - c.stars[alcyone].pos) / units::kParsec : -1.0);
    const NamedPlace* orion = findPlace(c, "Orion Nebula (M42)");
    CHECK(orion && std::fabs(glm::length(orion->pos - sunPos) / units::kParsec - 388.0) < 1.0, "Orion Nebula at 388 pc");
    const NamedPlace* virgo = findPlace(c, "Virgo Cluster");
    const GalaxyRecord* m87 = findGalaxy(c, "Virgo A (M87)");
    CHECK(virgo && m87 && glm::length(virgo->pos - m87->model.center) < virgo->radiusMeters,
          "M87 lies within the Virgo Cluster place (%.2f Mpc from its centre)",
          virgo && m87 ? glm::length(virgo->pos - m87->model.center) / (1e6 * units::kParsec) : -1.0);
    const NamedPlace* coma = findPlace(c, "Coma Cluster");
    const double comaMpc = coma ? glm::length(coma->pos - sunPos) / (1e6 * units::kParsec) : 0.0;
    // Same formula as the fetch script, which placed the cluster's members at 102.2 Mpc.
    CHECK(std::fabs(comaMpc - 102.2) < 0.06, "Coma Cluster at %.2f Mpc (fetch script: 102.2)", comaMpc);
    // Comoving distance: low-redshift limit cz/H0 and the integral at z = 0.1 (Omega_m = 0.3: 418.5 Mpc).
    CHECK(std::fabs(real::comovingDistanceMpc(700.0) - 10.0) < 0.01 && std::fabs(real::comovingDistanceMpc(29979.2458) - 418.5) < 0.5,
          "comoving distance %.3f Mpc at 700 km/s, %.1f Mpc at z = 0.1", real::comovingDistanceMpc(700.0),
          real::comovingDistanceMpc(29979.2458));
    // Coma members (NGC 4874 / 4889) sit inside the place.
    const GalaxyRecord* n4889 = findGalaxy(c, "NGC 4889");
    CHECK(coma && n4889 && glm::length(n4889->model.center - coma->pos) < coma->radiusMeters, "NGC 4889 inside the Coma Cluster place");
    const NamedPlace* lg = findPlace(c, "Local Group");
    const GalaxyRecord* m31 = findGalaxy(c, "Andromeda Galaxy (M31)");
    CHECK(lg && m31 && glm::length(lg->pos * 2.0 - m31->model.center) < 1e3 * units::kParsec, "Local Group half way to M31");
}

static void testPlacements(const std::string& assets) {
    std::printf("black hole placements\n");
    UniverseContent gc;
    std::string err;
    const bool okGc = buildRealUniverse(gc, assets, BlackHolePlacement::GalacticCenter, &err);
    CHECK(okGc && gc.blackHolePos == dvec3(0.0) && gc.suggestedMassSolar == 4.297e6 && gc.blackHoleName == "Sagittarius A*",
          "Galactic Centre: Sagittarius A* at the origin, 4.297e6 M_sun");
    const double tilt = angleDeg(gc.blackHoleSpinAxis, dvec3(0, 1, 0));
    CHECK(std::fabs(glm::length(gc.blackHoleSpinAxis) - 1.0) < 1e-12 && std::fabs(tilt - 50.0) < 1e-6,
          "Sgr A* spin axis %.2f deg from galactic north", tilt);
    const dvec3 sunPos = real::sunGalactocentricPc() * units::kParsec;
    const double inclSun = angleDeg(gc.blackHoleSpinAxis, sunPos - gc.blackHolePos);
    CHECK(inclSun < 50.0, "seen from the Sun the Sgr A* disk is inclined %.1f deg (EHT favours < 50)", inclSun);

    UniverseContent ns;
    const bool okNs = buildRealUniverse(ns, assets, BlackHolePlacement::NearSun, &err);
    const dvec3 rel = ns.blackHolePos - sunPos;
    CHECK(okNs && ns.blackHoleName == "Gargantua" && ns.suggestedMassSolar == 1e8 &&
          std::fabs(glm::length(rel) / units::kLightYear - 30.0) < 1e-9, "Near the Sun: Gargantua, 1e8 M_sun, %.3f ly from the Sun",
          glm::length(rel) / units::kLightYear);
    double minSep = 180.0, nearest = 1e30;
    for (size_t i = 1; i < ns.stars.size(); ++i) {
        const StarRecord& s = ns.stars[i];
        if (appMagFrom(s, sunPos) < 3.0) minSep = std::min(minSep, angleDeg(s.pos - sunPos, rel));
        nearest = std::min(nearest, glm::length(s.pos - ns.blackHolePos));
    }
    CHECK(minSep > 20.0, "Gargantua is %.1f deg from the nearest star brighter than V = 3", minSep);
    CHECK(nearest / units::kParsec > 2.0, "nearest catalogue star %.2f pc from Gargantua", nearest / units::kParsec);
    const double tiltNs = angleDeg(ns.blackHoleSpinAxis, dvec3(0, 1, 0));
    const double inclNs = angleDeg(ns.blackHoleSpinAxis, -rel);
    CHECK(std::fabs(tiltNs - 40.0) < 1e-6 && std::fabs(std::min(inclNs, 180 - inclNs) - 80.0) < 0.5,
          "Gargantua spin %.1f deg from galactic north, disk inclined %.1f deg as seen from the Sun", tiltNs, inclNs);
    const NamedPlace* gp = findPlace(ns, "Gargantua");
    CHECK(gp && gp->pos == ns.blackHolePos, "Gargantua is a named place");
    CHECK(gc.stars.size() == ns.stars.size() && gc.galaxies.size() == ns.galaxies.size(), "placements share the catalogue content");
}

// Robustness: missing, truncated, corrupt and partially invalid files.
static std::vector<unsigned char> readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void writeAll(const std::filesystem::path& p, const std::vector<unsigned char>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
static void fixCrc(std::vector<unsigned char>& b) {
    const uint32_t crc = real::crc32(b.data() + 64, b.size() - 64);
    static_assert(offsetof(real::StarFileHeader, payloadCrc) == 32 && offsetof(real::GalaxyFileHeader, payloadCrc) == 32);
    std::memcpy(b.data() + 32, &crc, 4);
}

static void testRobustness(const std::string& catalogDir) {
    std::printf("robustness\n");
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("bhs_real_universe_test_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    const auto stars = readAll(fs::path(catalogDir) / "stars.bin");
    const auto gals = readAll(fs::path(catalogDir) / "galaxies.bin");
    const auto attempt = [&](const std::vector<unsigned char>& s, const std::vector<unsigned char>& g, std::string& err) {
        writeAll(tmp / "stars.bin", s);
        writeAll(tmp / "galaxies.bin", g);
        UniverseContent c;
        c.blackHoleName = "untouched";
        err.clear();
        const bool ok = buildRealUniverse(c, tmp.string(), BlackHolePlacement::GalacticCenter, &err);
        if (!ok && c.blackHoleName != "untouched") err = "output modified on failure";
        return ok;
    };
    std::string err;
    UniverseContent c;
    c.blackHoleName = "untouched";
    const bool missing = buildRealUniverse(c, (tmp / "nowhere").string(), BlackHolePlacement::GalacticCenter, &err);
    CHECK(!missing && !err.empty() && c.blackHoleName == "untouched", "missing directory fails cleanly: %s", err.c_str());

    auto s = stars;
    s.pop_back();
    CHECK(!attempt(s, gals, err) && err.find("size") != std::string::npos, "truncated stars.bin rejected: %s", err.c_str());
    s = stars;
    s[s.size() / 2] ^= 0x40;
    CHECK(!attempt(s, gals, err) && err.find("checksum") != std::string::npos, "corrupt stars.bin rejected: %s", err.c_str());
    s = stars;
    s[0] = 'X';
    CHECK(!attempt(s, gals, err) && err.find("magic") != std::string::npos, "wrong file type rejected: %s", err.c_str());
    s = stars;
    s[8] = 2;
    CHECK(!attempt(s, gals, err) && err.find("version") != std::string::npos, "future format version rejected: %s", err.c_str());
    auto g = gals;
    g.resize(30);
    CHECK(!attempt(stars, g, err) && !err.empty(), "truncated galaxies.bin rejected: %s", err.c_str());
    g = gals;
    g.insert(g.end(), {1, 2, 3, 4});
    CHECK(!attempt(stars, g, err) && err.find("size") != std::string::npos, "galaxies.bin with trailing bytes rejected: %s", err.c_str());

    // A NaN in one star and one galaxy record with a valid checksum: those records are skipped.
    s = stars;
    const float nan = std::nanf("");
    std::memcpy(s.data() + 64 + 5 * 36 + 12, &nan, 4);     // absMag of star 5
    fixCrc(s);
    g = gals;
    std::memcpy(g.data() + 64 + 7 * 60 + 12, &nan, 4);     // diameter of galaxy 7
    fixCrc(g);
    const bool partial = attempt(s, g, err);
    CHECK(partial && real::lastLoadReport().starsSkipped == 1 && real::lastLoadReport().galaxiesSkipped == 1,
          "invalid records skipped, the rest loads (%zu stars, %zu galaxies skipped)", real::lastLoadReport().starsSkipped,
          real::lastLoadReport().galaxiesSkipped);
    // A star placed 500 kpc away (no Milky Way star is) is skipped too.
    s = stars;
    const float far = 5.0e5f;
    std::memcpy(s.data() + 64 + 9 * 36 + 0, &far, 4);      // x of star 9
    fixCrc(s);
    CHECK(attempt(s, gals, err) && real::lastLoadReport().starsSkipped == 1, "a star 500 kpc away is rejected as corrupt");
    // A corrupt name table (offsets out of order) is rejected.
    s = stars;
    {
        real::StarFileHeader h;
        std::memcpy(&h, s.data(), sizeof h);
        const size_t offs = 64 + size_t(h.count) * 36;
        const uint32_t big = h.nameBytes + 5;
        std::memcpy(s.data() + offs + 4 * 10, &big, 4);
        fixCrc(s);
    }
    CHECK(!attempt(s, gals, err) && err.find("name") != std::string::npos, "corrupt name table rejected: %s", err.c_str());
    // The asset directory or its "real" subdirectory both work.
    fs::create_directories(tmp / "nested" / "real");
    writeAll(tmp / "nested" / "real" / "stars.bin", stars);
    writeAll(tmp / "nested" / "real" / "galaxies.bin", gals);
    UniverseContent nested;
    CHECK(buildRealUniverse(nested, (tmp / "nested").string(), BlackHolePlacement::NearSun, &err), "assetDir/real is found");
    fs::remove_all(tmp);
}

static uint64_t fingerprint(const UniverseContent& c) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const StarRecord& s : c.stars) mix(&s, sizeof s);
    for (const GalaxyRecord& g : c.galaxies) {
        mix(&g.model.center, sizeof g.model.center);
        mix(&g.model.normal, sizeof g.model.normal);
        mix(&g.model.diskScaleLengthPc, sizeof(double));
        mix(&g.model.seed, sizeof g.model.seed);
    }
    for (const std::string& n : c.names) mix(n.data(), n.size());
    return h;
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const std::string assets = argc > 1 ? argv[1] : (fs::path(__FILE__).parent_path().parent_path() / "assets").string();
    std::printf("assets: %s\n", assets.c_str());
    testFrames();
    testOrbits();

    UniverseContent c;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = buildRealUniverse(c, assets, BlackHolePlacement::GalacticCenter, &err);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ok, "buildRealUniverse succeeded %s", err.c_str());
    if (!ok) {
        std::printf("FAILED: catalogues missing; run tools/fetch_real_catalogs.py\n");
        return 1;
    }
    {
        const fs::path dir = real::resolveCatalogDir(assets);
        std::string e1, e2;
        const bool raw = real::parseStarFile(readAll(dir / "stars.bin"), g_starFile, e1) &&
                         real::parseGalaxyFile(readAll(dir / "galaxies.bin"), g_galaxyFile, e2);
        CHECK(raw, "raw catalogue files parse %s%s", e1.c_str(), e2.c_str());
        if (!raw) return 1;
    }
    testContent(c, secs);
    if (g_starFile.records.size() == c.stars.size() && g_galaxyFile.records.size() == c.galaxies.size()) {
        testDustColumn(c);
        testCatalogueFixes(c);
    }
    testMilkyWay(c);
    testSolarSystem(c);
    testPlaces(c);
    testPlacements(assets);
    testRobustness(real::resolveCatalogDir(assets));

    UniverseContent c2;
    buildRealUniverse(c2, assets, BlackHolePlacement::GalacticCenter, &err);
    CHECK(fingerprint(c) == fingerprint(c2), "loading twice gives identical content");

    size_t nameBytes = 0;
    for (const std::string& n : c.names) nameBytes += n.capacity() + sizeof(std::string);
    const double mb = (c.stars.size() * sizeof(StarRecord) + c.galaxies.size() * sizeof(GalaxyRecord) + nameBytes) / 1048576.0;
    std::printf("memory: %zu stars x %zu B + %zu galaxies x %zu B + names %.1f MB = %.1f MB; load %.0f ms\n", c.stars.size(),
                sizeof(StarRecord), c.galaxies.size(), sizeof(GalaxyRecord), nameBytes / 1048576.0, mb, secs * 1e3);
    std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "FAILED" : "all real universe checks passed");
    return g_failures ? 1 : 0;
}
