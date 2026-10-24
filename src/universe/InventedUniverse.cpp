// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
//
// Invented universe: a deterministic, seed-based universe around the black hole. A Milky-Way-sized
// spiral hosts the black hole on one of its arms, with a few notable named stars nearby (the rest
// of its stars come from the procedural StarField); satellite dwarfs and a large neighbour spiral
// form a local group, and ~30k galaxies fill a cosmic web of clusters, groups and filaments out to
// 150 Mpc.
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "Units.h"
#include "universe/Stars_internal.h"
#include "universe/Universe.h"

namespace bhs::universe {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPc = units::kParsec;
constexpr double kKpc = 1e3 * units::kParsec;
constexpr double kMpc = 1e6 * units::kParsec;
constexpr double kLStar = 2.5e10;   // Schechter characteristic luminosity (L_sun, V band)

using detail::Rng;

glm::dvec3 randomUnit(Rng& r) {
    const double z = r.range(-1.0, 1.0), phi = r.range(0.0, 2.0 * kPi), s = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {s * std::cos(phi), s * std::sin(phi), z};
}

glm::dvec3 perpendicular(const glm::dvec3& n, Rng& r) {
    const glm::dvec3 a = std::abs(n.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 u = glm::normalize(glm::cross(n, a)), v = glm::cross(n, u);
    const double t = r.range(0.0, 2.0 * kPi);
    return std::cos(t) * u + std::sin(t) * v;
}

class NameMaker {
public:
    explicit NameMaker(Rng& r) : r_(r) {}
    std::string proper() {
        static const char* first[] = {"Al", "Be", "Ca", "Da", "El", "Fa", "Ga", "Ha", "Ith", "Ka", "La", "Ma", "Na", "Or",
                                      "Pe", "Qua", "Ra", "Sa", "Ta", "Ul", "Va", "Xe", "Ya", "Ze", "Thal", "Vor", "Myr",
                                      "Kel", "Ser", "Ae", "Ori", "Zha", "Lyr", "Cor", "Hel", "Nyx", "Sol", "Tor", "Ves"};
        static const char* mid[] = {"ra", "ni", "to", "ve", "li", "ma", "ro", "sa", "ke", "di", "lo", "ne",
                                    "ri", "va", "the", "dra", "mi", "ga", "sha", "ty", "len", "cor"};
        static const char* last[] = {"n", "s", "x", "r", "th", "l", "ra", "ris", "nus", "na", "ia", "on",
                                     "ar", "el", "is", "ax", "or", "us", "ea", "yx", "ion", "ara"};
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::string n = first[r_.next() % (sizeof(first) / sizeof(first[0]))];
            if (r_.uniform() < 0.75) n += mid[r_.next() % (sizeof(mid) / sizeof(mid[0]))];
            n += last[r_.next() % (sizeof(last) / sizeof(last[0]))];
            if (used_.insert(n).second) return n;
        }
        return "Unnamed " + std::to_string(used_.size());
    }
    std::string catalogue(const char* prefix) { return std::string(prefix) + " " + std::to_string(1000 + (counter_++) * 7 % 90000); }

private:
    Rng& r_;
    std::set<std::string> used_;
    int counter_ = 1;
};

int32_t addName(UniverseContent& out, const std::string& n) {
    out.names.push_back(n);
    return static_cast<int32_t>(out.names.size() - 1);
}

// Disk galaxy with randomised, plausible structure for its luminosity.
GalaxyModel diskGalaxy(Rng& r, double lum, bool barredAllowed, bool irregular) {
    GalaxyModel m;
    const double x = lum / kLStar;
    m.seed = static_cast<uint32_t>(r.next());
    m.luminositySolar = lum;
    m.stellarMassSolar = lum * (irregular ? 1.0 : r.range(1.5, 2.3));
    if (irregular) {
        m.type = GalaxyType::Irregular;
        m.diskScaleLengthPc = 1500.0 * std::pow(x, 0.35) * r.range(0.8, 1.25);
        m.diskScaleHeightPc = m.diskScaleLengthPc * r.range(0.25, 0.4);
        m.bulgeMassFraction = 0.0;
        m.armCount = 2;
        m.armContrast = 1.3;
        m.armPitchDeg = 25.0;
        m.dustExtinctionPerKpc = 0.8;
        m.dustScaleHeightPc = 0.5 * m.diskScaleHeightPc;
        m.diskColor = glm::vec3(0.85f, 0.9f, 1.0f);
    } else {
        const bool barred = barredAllowed && r.uniform() < 0.45;
        m.type = barred ? GalaxyType::BarredSpiral : GalaxyType::Spiral;
        m.diskScaleLengthPc = 3200.0 * std::pow(x, 0.35) * r.range(0.8, 1.25);
        m.diskScaleHeightPc = m.diskScaleLengthPc * r.range(0.09, 0.13);
        m.bulgeMassFraction = r.range(0.08, 0.3);
        m.bulgeRadiusPc = m.diskScaleLengthPc * r.range(0.22, 0.38);
        m.bulgeFlattening = r.range(0.55, 0.8);
        m.armCount = (barred || r.uniform() < 0.6) ? 2 : (r.uniform() < 0.5 ? 3 : 4);
        m.armPitchDeg = r.range(10.0, 22.0);
        m.armWidth = r.range(0.3, 0.42);
        m.armContrast = r.range(2.4, 3.6);
        if (barred) {
            m.barLengthPc = m.diskScaleLengthPc * r.range(1.1, 1.8);
            m.barAxisRatio = r.range(0.3, 0.45);
            m.barAngleDeg = r.range(0.0, 360.0);
            m.armRefRadiusPc = m.barLengthPc;
            m.armPhaseDeg = m.barAngleDeg;
        } else {
            m.armRefRadiusPc = m.diskScaleLengthPc * r.range(0.9, 1.4);
            m.armPhaseDeg = r.range(0.0, 360.0);
        }
        m.dustScaleHeightPc = m.diskScaleHeightPc * r.range(0.3, 0.45);
        m.dustExtinctionPerKpc = r.range(2.5, 5.0) * 300.0 / std::max(m.diskScaleHeightPc, 50.0);
        const float warm = static_cast<float>(r.range(0.0, 1.0));
        m.diskColor = glm::mix(glm::vec3(0.92f, 0.92f, 1.0f), glm::vec3(1.0f, 0.9f, 0.8f), warm * 0.6f);
    }
    m.diskCutoffPc = m.diskScaleLengthPc * r.range(4.5, 5.8);
    return m;
}

GalaxyModel ellipticalGalaxy(Rng& r, double lum, bool lenticular) {
    GalaxyModel m;
    const double x = lum / kLStar;
    m.seed = static_cast<uint32_t>(r.next());
    m.luminositySolar = lum;
    m.stellarMassSolar = lum * r.range(3.0, 4.5);
    if (lenticular) {
        m.type = GalaxyType::Lenticular;
        m.diskScaleLengthPc = 2800.0 * std::pow(x, 0.35) * r.range(0.8, 1.2);
        m.diskScaleHeightPc = m.diskScaleLengthPc * r.range(0.12, 0.2);
        m.diskCutoffPc = 4.5 * m.diskScaleLengthPc;
        m.bulgeMassFraction = r.range(0.35, 0.6);
        m.bulgeRadiusPc = m.diskScaleLengthPc * r.range(0.3, 0.5);
        m.armContrast = 1.0;
        m.dustExtinctionPerKpc = r.range(0.0, 1.0);
        m.dustScaleHeightPc = 0.3 * m.diskScaleHeightPc;
        m.diskColor = glm::vec3(1.0f, 0.86f, 0.7f);
    } else {
        m.type = GalaxyType::Elliptical;
        m.ellipticalRadiusPc = 4500.0 * std::pow(x, 0.6) * r.range(0.8, 1.25);
        m.ellipticalAxisRatio = r.range(0.45, 0.95);
        m.bulgeMassFraction = 1.0;
        m.dustExtinctionPerKpc = 0.0;
        m.diskColor = glm::vec3(1.0f, 0.82f, 0.62f);
    }
    m.bulgeColor = x > 1.0 ? glm::vec3(1.0f, 0.78f, 0.56f) : glm::vec3(1.0f, 0.83f, 0.64f);
    return m;
}

// Schechter luminosity function (alpha = -1.15) between lo and hi, by rejection from a power law.
double schechterLuminosity(Rng& r, double lo, double hi) {
    const double a = -1.15 + 1.0;   // exponent of the sampling power law, L^(alpha) dL -> L^(alpha+1) d ln L
    for (int i = 0; i < 1000; ++i) {
        const double l = r.logRange(lo, hi);
        // density per d ln L: (L/L*)^(alpha+1) exp(-L/L*); sample log-uniform and accept.
        const double p = std::pow(l / lo, a) * std::exp(-(l - lo) / kLStar);
        if (r.uniform() < p) return l;
    }
    return lo;
}

void orient(GalaxyModel& m, Rng& r) {
    m.normal = randomUnit(r);
    m.major = perpendicular(m.normal, r);
}

struct Node {
    glm::dvec3 pos;
    double richness;   // number of member galaxies
    double radius;     // virial radius (m)
};

}  // namespace

void buildInventedUniverse(UniverseContent& out, uint32_t seed) {
    out = UniverseContent{};
    out.mode = Mode::Invented;
    Rng r(static_cast<uint64_t>(seed) * 0x2545F4914F6CDD1Dull + 0x1234567ull);
    NameMaker names(r);

    // ---------------------------------------------------------------- host galaxy
    {
        GalaxyModel& h = out.host;
        h = diskGalaxy(r, r.logRange(2.2e10, 3.8e10), true, false);
        h.center = glm::dvec3(0.0);
        h.normal = glm::dvec3(0.0, 1.0, 0.0);
        h.major = glm::dvec3(1.0, 0.0, 0.0);
        h.diskScaleLengthPc = r.range(2400.0, 3300.0);
        h.diskScaleHeightPc = r.range(270.0, 330.0);
        h.diskCutoffPc = h.diskScaleLengthPc * r.range(5.2, 5.8);
        h.bulgeRadiusPc = r.range(650.0, 950.0);
        if (h.type == GalaxyType::BarredSpiral) {
            h.barLengthPc = h.diskScaleLengthPc * r.range(1.3, 1.8);
            h.armRefRadiusPc = h.barLengthPc;
            h.armPhaseDeg = h.barAngleDeg;
        } else {
            h.armRefRadiusPc = h.diskScaleLengthPc * r.range(1.0, 1.4);
        }
        h.armPitchDeg = r.range(11.0, 17.0);
        h.armContrast = r.range(2.8, 3.6);
        h.dustScaleHeightPc = r.range(95.0, 125.0);
        h.dustExtinctionPerKpc = r.range(2.8, 4.0);
    }
    const GalaxyModel& host = out.host;
    const std::string hostName = names.proper();

    // ---------------------------------------------------------------- the black hole, on an arm
    {
        const double R = r.range(4.2, 6.8) * kKpc;
        const double armRef = std::max(host.armRefRadiusPc, 100.0) * kPc;
        const double invTanP = 1.0 / std::tan(host.armPitchDeg * kPi / 180.0);
        const double psi = r.range(-0.35, 0.55);   // on the arm, possibly by its inner dust lane
        const int arm = static_cast<int>(r.next() % static_cast<uint64_t>(std::max(host.armCount, 1)));
        const double theta = (psi + 2.0 * kPi * arm) / std::max(host.armCount, 1) + std::log(R / armRef) * invTanP +
                             host.armPhaseDeg * kPi / 180.0;
        const glm::dvec3 minor = glm::cross(host.normal, host.major);
        out.blackHolePos = host.center + R * (std::cos(theta) * host.major + std::sin(theta) * minor) +
                           host.normal * (r.normal() * 15.0 * kPc);
        // The spin axis tilts at right angles to the direction of the galactic centre, so the line where
        // the accretion disk's plane meets the galaxy's points at the centre: seen along it (the default
        // views), the galaxy's band and bulge lie right behind the black hole, crossing the disk at the tilt.
        const double tilt = r.range(35.0, 70.0) * kPi / 180.0;
        glm::dvec3 toCentre = host.center - out.blackHolePos;
        toCentre = glm::normalize(toCentre - glm::dot(toCentre, host.normal) * host.normal);
        const double side = r.uniform() < 0.5 ? 1.0 : -1.0;
        out.blackHoleSpinAxis = glm::normalize(std::cos(tilt) * host.normal + side * std::sin(tilt) * glm::cross(toCentre, host.normal));
        out.suggestedMassSolar = 0.0;
        out.blackHoleName = "Gargantua";
    }
    out.places.push_back({"Galactic centre", host.center, 3000.0 * kPc});

    // ---------------------------------------------------------------- notable stars nearby
    {
        struct Kind {
            const char* role;
            double magLo, magHi, teffLo, teffHi, distLo, distHi;   // M_V, K, pc
        };
        const Kind kinds[] = {
            {"blue supergiant", -8.0, -6.8, 14000, 24000, 15, 45},
            {"red supergiant", -6.5, -5.5, 3400, 3800, 10, 40},
            {"orange giant", -0.6, 0.6, 4100, 4600, 3, 15},
            {"Sun-like star", 4.5, 5.1, 5600, 5900, 0.4, 2.5},
            {"red dwarf", 11.0, 13.0, 3000, 3400, 0.25, 1.2},
            {"white dwarf", 11.0, 12.5, 9000, 16000, 0.8, 4},
            {"white A star", 1.2, 2.0, 8600, 9800, 3, 12},
            {"blue-white B star", -2.0, -1.0, 13000, 18000, 8, 25},
            {"yellow-white F star", 2.8, 3.6, 6400, 7000, 1.5, 6},
        };
        for (const Kind& k : kinds) {
            StarRecord s;
            const double d = r.logRange(k.distLo, k.distHi) * kPc;
            glm::dvec3 dir = randomUnit(r);
            dir = glm::normalize(dir - 0.6 * glm::dot(dir, host.normal) * host.normal);   // favour the disk plane
            s.pos = out.blackHolePos + dir * d;
            s.absMag = static_cast<float>(r.range(k.magLo, k.magHi));
            s.teff = static_cast<float>(r.range(k.teffLo, k.teffHi));
            s.radiusSolar = std::string(k.role) == "white dwarf"
                                ? static_cast<float>(r.range(0.009, 0.014))
                                : static_cast<float>(detail::stellarRadiusSolar(s.absMag, s.teff));
            if (std::string(k.role) == "white dwarf")   // keep magnitude consistent with the tiny radius
                s.absMag = static_cast<float>(4.74 - 2.5 * std::log10(std::pow(s.radiusSolar, 2.0) * std::pow(s.teff / 5772.0, 4.0)) -
                                              detail::bolometricCorrectionV(s.teff));
            s.name = addName(out, names.proper());
            out.stars.push_back(s);
        }
        // A close binary: a Sun-like star and an orange dwarf 300 AU apart.
        const double d = r.logRange(1.0, 4.0) * kPc;
        const glm::dvec3 c = out.blackHolePos + randomUnit(r) * d;
        const glm::dvec3 sep = randomUnit(r) * (150.0 * units::kAU);
        const std::string bn = names.proper();
        StarRecord a, b;
        a.pos = c + sep;
        a.absMag = static_cast<float>(r.range(4.4, 4.9));
        a.teff = static_cast<float>(r.range(5700, 6000));
        a.radiusSolar = static_cast<float>(detail::stellarRadiusSolar(a.absMag, a.teff));
        a.name = addName(out, bn + " A");
        b.pos = c - sep;
        b.absMag = static_cast<float>(r.range(6.2, 7.0));
        b.teff = static_cast<float>(r.range(4400, 4900));
        b.radiusSolar = static_cast<float>(detail::stellarRadiusSolar(b.absMag, b.teff));
        b.name = addName(out, bn + " B");
        out.stars.push_back(a);
        out.stars.push_back(b);
    }

    // ---------------------------------------------------------------- local group
    const int satellites = 2 + static_cast<int>(r.next() % 4);
    for (int i = 0; i < satellites; ++i) {
        const bool irr = i == 0 || r.uniform() < 0.5;   // the biggest is a Magellanic-type irregular
        const double lum = i == 0 ? r.logRange(5e8, 2.5e9) : r.logRange(5e6, 4e8);
        GalaxyRecord g;
        g.model = irr ? diskGalaxy(r, lum, false, true) : ellipticalGalaxy(r, lum, false);
        if (!irr) g.model.ellipticalRadiusPc = 300.0 * std::pow(lum / 1e7, 0.4) * r.range(0.7, 1.4);
        orient(g.model, r);
        g.model.center = host.center + randomUnit(r) * (r.logRange(50.0, 300.0) * kKpc);
        g.absMag = static_cast<float>(units::kSolarAbsMagV - 2.5 * std::log10(lum));
        g.name = addName(out, names.proper() + (irr ? " Cloud" : " Dwarf"));
        out.galaxies.push_back(g);
    }
    {
        GalaxyRecord g;
        g.model = diskGalaxy(r, r.logRange(4e10, 6.5e10), false, false);
        g.model.type = GalaxyType::Spiral;
        g.model.barLengthPc = 0.0;
        g.model.armCount = 2;
        g.model.bulgeMassFraction = r.range(0.25, 0.35);
        g.model.dustExtinctionPerKpc = r.range(3.0, 5.0) * 300.0 / g.model.diskScaleHeightPc;
        orient(g.model, r);
        g.model.center = host.center + randomUnit(r) * (r.range(0.7, 1.5) * kMpc);
        g.absMag = static_cast<float>(units::kSolarAbsMagV - 2.5 * std::log10(g.model.luminositySolar));
        g.name = addName(out, names.proper() + " Galaxy");
        out.galaxies.push_back(g);
    }

    // ---------------------------------------------------------------- cosmic web
    const double rMax = 150.0 * kMpc;
    std::vector<Node> nodes;
    {
        // The nearest rich cluster, Virgo-like.
        Node v;
        v.pos = randomUnit(r) * (r.range(15.0, 25.0) * kMpc);
        v.richness = r.range(350.0, 550.0);
        v.radius = 2.2 * kMpc;
        nodes.push_back(v);
        const std::string cn = names.proper() + " Cluster";
        out.places.push_back({cn, v.pos, 1.6 * kMpc});
        // Groups and clusters: richness ~ power law, positions uniform in volume beyond the local group.
        while (nodes.size() < 1100) {
            Node n;
            const double d = rMax * std::cbrt(r.uniform());
            if (d < 4.0 * kMpc) continue;
            n.pos = randomUnit(r) * d;
            n.richness = 5.0 / std::pow(std::max(r.uniform(), 1e-6), 1.0 / 1.1);   // p(N) ~ N^-2.1
            n.richness = std::min(n.richness, 400.0);
            n.radius = 0.35 * kMpc * std::cbrt(n.richness / 5.0);
            nodes.push_back(n);
        }
    }
    const auto addGalaxy = [&](const glm::dvec3& pos, double earlyTypeChance, double lum, bool brightest) {
        GalaxyRecord g;
        const double u = r.uniform();
        if (brightest || u < earlyTypeChance) {
            g.model = ellipticalGalaxy(r, lum, !brightest && r.uniform() < 0.35);
        } else {
            g.model = diskGalaxy(r, lum, true, lum < 0.15 * kLStar && r.uniform() < 0.5);
        }
        orient(g.model, r);
        g.model.center = pos;
        g.absMag = static_cast<float>(units::kSolarAbsMagV - 2.5 * std::log10(lum));
        out.galaxies.push_back(g);
    };
    // Cluster and group members.
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        const Node& n = nodes[ni];
        const int members = static_cast<int>(n.richness * (ni == 0 ? 1.0 : 0.45));
        for (int m = 0; m < members; ++m) {
            const bool bcg = m == 0 && n.richness > 60.0;
            const double rr = n.radius * std::pow(r.uniform(), 1.6);   // concentrated towards the core
            const glm::dvec3 pos = n.pos + randomUnit(r) * rr;
            const double core = rr / n.radius;
            const double early = n.richness > 60.0 ? (core < 0.4 ? 0.75 : 0.5) : 0.35;
            const double lum = bcg ? r.logRange(5.0, 15.0) * kLStar : schechterLuminosity(r, 0.05 * kLStar, 6.0 * kLStar);
            addGalaxy(pos, early, lum, bcg);
        }
    }
    // Filaments between neighbouring nodes.
    for (size_t i = 0; i < nodes.size(); ++i) {
        size_t best[2] = {i, i};
        double bd[2] = {1e300, 1e300};
        for (size_t j = 0; j < nodes.size(); ++j) {
            if (j == i) continue;
            const double d = glm::length(nodes[j].pos - nodes[i].pos);
            if (d < bd[0]) {
                bd[1] = bd[0];
                best[1] = best[0];
                bd[0] = d;
                best[0] = j;
            } else if (d < bd[1]) {
                bd[1] = d;
                best[1] = j;
            }
        }
        for (int k = 0; k < 2; ++k) {
            if (best[k] < i || bd[k] > 30.0 * kMpc) continue;   // each pair once, short links only
            const glm::dvec3 a = nodes[i].pos, b = nodes[best[k]].pos;
            const int count = static_cast<int>(bd[k] / kMpc * 0.9);
            for (int m = 0; m < count; ++m) {
                const glm::dvec3 p = glm::mix(a, b, r.uniform()) + randomUnit(r) * (r.normal() * 0.8 * kMpc);
                addGalaxy(p, 0.3, schechterLuminosity(r, 0.05 * kLStar, 5.0 * kLStar), false);
            }
        }
    }
    // Field galaxies and a sprinkling of nearby dwarfs.
    for (int i = 0; i < 3500; ++i) {
        const double d = rMax * std::cbrt(r.uniform());
        if (d < 2.0 * kMpc) continue;
        addGalaxy(randomUnit(r) * d, 0.2, schechterLuminosity(r, 0.05 * kLStar, 4.0 * kLStar), false);
    }
    for (int i = 0; i < 40; ++i) {
        const double d = r.range(1.5, 8.0) * kMpc;
        addGalaxy(randomUnit(r) * d, 0.25, schechterLuminosity(r, 0.005 * kLStar, 1.5 * kLStar), false);
    }

    // Names for the galaxies that look most impressive from home: proper names for the brightest,
    // catalogue numbers for the next few hundred.
    std::vector<std::pair<double, size_t>> bright;
    for (size_t i = 0; i < out.galaxies.size(); ++i) {
        if (out.galaxies[i].name >= 0) continue;
        const double d = std::max(glm::length(out.galaxies[i].model.center - out.blackHolePos) / kPc, 1.0);
        bright.push_back({out.galaxies[i].absMag + 5.0 * std::log10(d / 10.0), i});
    }
    std::sort(bright.begin(), bright.end());
    for (size_t k = 0; k < bright.size() && k < 300; ++k) {
        GalaxyRecord& g = out.galaxies[bright[k].second];
        g.name = addName(out, k < 25 ? names.proper() + (g.model.type == GalaxyType::Elliptical ? " Elliptical" : " Galaxy")
                                     : names.catalogue("IGC"));
    }
    (void)hostName;
}

}  // namespace bhs::universe
