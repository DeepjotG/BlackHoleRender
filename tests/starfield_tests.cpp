// Checks for the procedural star field (src/universe/StarField.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Units.h"
#include "universe/StarField.h"
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

namespace {
// Gathers until every cell within reach has been generated and the result is published.
std::vector<StarRecord> settle(StarField& f, const glm::dvec3& at, float mlim, size_t cap = 400000) {
    std::vector<StarRecord> out;
    size_t lastCells = 0;
    int stable = 0;
    for (int i = 0; i < 5000 && stable < 12; ++i) {
        f.gather(at, mlim, cap, out);
        stable = f.cachedCells() == lastCells ? stable + 1 : 0;
        lastCells = f.cachedCells();
    }
    return out;
}
bool samePositions(std::vector<StarRecord> a, std::vector<StarRecord> b) {
    if (a.size() != b.size()) return false;
    const auto less = [](const StarRecord& p, const StarRecord& q) {
        if (p.pos.x != q.pos.x) return p.pos.x < q.pos.x;
        if (p.pos.y != q.pos.y) return p.pos.y < q.pos.y;
        return p.pos.z < q.pos.z;
    };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].pos != b[i].pos || a[i].absMag != b[i].absMag) return false;
    return true;
}
}  // namespace

int main() {
    UniverseContent u;
    buildInventedUniverse(u, 2024u);
    const glm::dvec3 home = u.blackHolePos;
    const float mlim = 7.0f;

    std::printf("Determinism\n");
    StarField a, b;
    a.configure(u, 9u);
    b.configure(u, 9u);
    const std::vector<StarRecord> sa = settle(a, home, mlim), sb = settle(b, home, mlim);
    CHECK(!sa.empty() && samePositions(sa, sb), "two fields with the same seed give the same %zu stars", sa.size());
    StarField c;
    c.configure(u, 10u);
    CHECK(!samePositions(sa, settle(c, home, mlim)), "another seed gives other stars");
    StarField d;
    d.configure(u, 9u);
    settle(d, home + glm::dvec3(30.0 * units::kParsec, 0.0, 0.0), mlim);
    CHECK(samePositions(sa, settle(d, home, mlim)), "stars do not depend on where the field was seen from before");

    std::printf("Magnitudes, cap and physical values\n");
    {
        double worst = -100.0;
        bool sane = true;
        for (const StarRecord& s : sa) {
            const double dPc = glm::length(s.pos - home) / units::kParsec;
            worst = std::max(worst, s.absMag + 5.0 * std::log10(std::max(dPc, 1e-3) / 10.0));
            sane = sane && std::isfinite(s.teff) && s.teff > 2000.0f && s.teff < 50000.0f && s.radiusSolar > 0.0f && s.radiusSolar < 3000.0f &&
                   s.absMag > -9.0f && s.absMag < 17.0f;
        }
        // A class spans one magnitude, and with the cell and the viewpoint's sub-cell a published star can be
        // ~1.9 visibility radii away: up to ~2.4 mag fainter than the limit (the GPU applies the exact cut).
        CHECK(worst < mlim + 2.6, "published stars are close to the limit (faintest %.2f, limit %.1f)", worst, mlim);
        CHECK(sane, "temperatures, radii and magnitudes are physical");
        std::vector<StarRecord> capped;
        StarField e;
        e.configure(u, 9u);
        for (int i = 0; i < 400; ++i) e.gather(home, mlim, 1000, capped);
        CHECK(capped.size() <= 1000, "the cap is respected (%zu <= 1000)", capped.size());
        size_t hot = 0, cool = 0;
        for (const StarRecord& s : sa) (s.teff > 10000.0f ? hot : cool) += 1;
        CHECK(hot > 0 && cool > 0, "both hot young stars (%zu) and cool giants/dwarfs (%zu) appear", hot, cool);
    }

    std::printf("Counts versus density\n");
    {
        // Naked-eye-like counts from inside a Milky-Way-sized disk (the real sky: ~9000 stars to V = 6.5).
        StarField f;
        f.configure(u, 9u);
        const std::vector<StarRecord> s = settle(f, home, 6.5f);
        size_t visible = 0;
        for (const StarRecord& r : s) {
            const double dPc = glm::length(r.pos - home) / units::kParsec;
            if (r.absMag + 5.0 * std::log10(std::max(dPc, 1e-3) / 10.0) < 6.5) ++visible;
        }
        CHECK(visible > 1500 && visible < 60000, "%zu stars brighter than 6.5 before extinction (sky: ~9000 after it)", visible);
        // Far outside every galaxy there are no stars.
        StarField g;
        g.configure(u, 9u);
        const glm::dvec3 voidPoint = home + glm::normalize(glm::dvec3(0.3, 0.9, 0.2)) * (400.0 * 1e3 * units::kParsec);
        CHECK(settle(g, voidPoint, mlim).empty() || g.cachedCells() < 100, "intergalactic space is (nearly) empty");
    }

    std::printf("Catalogue suppression\n");
    {
        UniverseContent v = u;
        v.catalogCenter = home;
        v.catalogRadiusPc = 300.0;
        v.catalogLimitAppMag = 6.0f;
        StarField f;
        f.configure(v, 9u);
        bool none = true;
        for (const StarRecord& s : settle(f, home, mlim)) {
            const double dPc = glm::length(s.pos - home) / units::kParsec;
            if (dPc < 300.0 && s.absMag + 5.0 * std::log10(std::max(dPc, 1e-3) / 10.0) < 6.0) none = false;
        }
        CHECK(none, "no procedural star duplicates the catalogue region (r < 300 pc, brighter than 6)");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
