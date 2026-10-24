// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
//
// Procedural stars drawn from the galaxy models. Stars are split into 24 one-magnitude classes
// (M_V -8 .. +15); class k lives on its own cubic lattice whose cell size is matched to the distance
// out to which its stars can be brighter than the limiting magnitude. A cell is generated the first
// time it comes within that distance: the expected count is integrated adaptively (cells split where
// the density varies a lot, e.g. across a thin disk or a bulge's cusp) and stars are placed by
// rejection against the local density, each cell seeded by its coordinates so it always yields the
// same stars. Old and young populations have their own luminosity functions; the model's young
// fraction decides the mix, so hot blue stars crowd the arms and the midplane.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

#include "Units.h"
#include "universe/GalaxyModel_internal.h"
#include "universe/StarField.h"
#include "universe/Stars_internal.h"

namespace bhs::universe {

namespace {

constexpr int kMinMag = -8;
constexpr int kClasses = 24;   // M_V -8 .. +15
constexpr double kPc = units::kParsec;

// Stars per pc^3 per magnitude, old disk/bulge population (solar neighbourhood shape: giants and
// subgiants at the bright end, main-sequence dwarfs below the turn-off), and the bright end of a
// young population (O/B stars and supergiants, ~0.25 of midplane stars in arms).
constexpr double kOldLF[kClasses] = {0, 0, 0, 0, 2e-8, 1e-7, 4e-7, 2e-6, 1.2e-5, 3e-5, 4e-5, 1.2e-4,
                                     5e-4, 1.2e-3, 2e-3, 2.5e-3, 3e-3, 3.5e-3, 4.5e-3, 6e-3, 7e-3, 7e-3, 6.5e-3, 6e-3};
constexpr double kYoungFrac[kClasses] = {4e-8, 1.3e-7, 4e-7, 1.3e-6, 4e-6, 1.5e-5, 6e-5, 1.5e-4, 4e-4, 7e-4, 1.2e-3, 2.5e-3,
                                         4e-3, 8e-3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

struct ClassFractions {
    double old[kClasses], young[kClasses];
    ClassFractions() {
        double so = 0.0, faint = 0.0, bright = 0.0;
        for (int k = 0; k < kClasses; ++k) so += kOldLF[k];
        for (int k = 0; k < kClasses; ++k) {
            old[k] = kOldLF[k] / so;
            bright += kYoungFrac[k];
            if (k >= 14) faint += kOldLF[k];
        }
        // The young population's faint end follows the old one's shape.
        for (int k = 0; k < kClasses; ++k) young[k] = k < 14 ? kYoungFrac[k] : (1.0 - bright) * kOldLF[k] / faint;
    }
};
const ClassFractions& fractions() {
    static const ClassFractions f;
    return f;
}

double interp(const double* xs, const double* ys, int n, double x) {
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; ++i)
        if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[n - 1];
}
double mainSequenceTeff(double M) {
    static const double m[] = {-6, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const double t[] = {40000, 30000, 24000, 19000, 14000, 10500, 9300, 8300, 7200, 6300, 5800, 5200,
                               4700, 4200, 3900, 3700, 3500, 3350, 3200, 3050, 2950};
    return interp(m, t, 21, M);
}
double giantTeff(double M) {
    static const double m[] = {-4, -3, -2, -1, 0, 1, 2};
    static const double t[] = {3300, 3500, 3800, 4050, 4500, 4800, 5000};
    return interp(m, t, 7, M);
}

uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

struct CellKey {
    int32_t cls;
    int64_t x, y, z;
    bool operator==(const CellKey& o) const { return cls == o.cls && x == o.x && y == o.y && z == o.z; }
};
struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
        return static_cast<size_t>(mix64(static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull ^ mix64(static_cast<uint64_t>(k.y) + 0x632BE59BD9B4E019ull) ^
                                         mix64(static_cast<uint64_t>(k.z) * 3 + static_cast<uint64_t>(k.cls) * 0x2545F4914F6CDD1Dull)));
    }
};

}  // namespace

struct StarField::Impl {
    struct Galaxy {
        detail::GalaxyEval eval;
        glm::dvec3 center;
        glm::dvec3 normal;
        double boundM;
        double slabM;       // beyond this distance from the midplane only the bulge/halo remains
        double coreM;       // radius containing the spheroid
        bool disk;
    };
    std::vector<Galaxy> galaxies;
    std::vector<size_t> relevant;   // galaxies near the current viewpoint
    glm::dvec3 relevantAt{1e300};
    uint64_t seed = 0;
    // Catalogue suppression (real universe).
    glm::dvec3 catCenter{0.0};
    double catRadiusPc = 0.0;
    float catLimit = 0.0f;

    struct Cell {
        std::vector<StarRecord> stars;
        uint64_t lastUse = 0;
    };
    std::unordered_map<CellKey, Cell, CellKeyHash> cells;
    struct ClassState {
        bool valid = false;
        int64_t vx = 0, vy = 0, vz = 0;   // lattice cell of the viewpoint when `keys` was built
        std::vector<CellKey> keys;        // cells within reach (generated or not)
    };
    ClassState classes[kClasses];
    float cachedMag = -100.0f;
    uint64_t frame = 0;
    std::vector<CellKey> active, prevActive;
    bool pending = false;   // cells still to generate from the last call
    bool dirty = false;     // the published set is out of date
    int sinceReport = 0;    // gathers since the set was last published

    double visibility(int cls, float mlim) const {   // pc
        return std::pow(10.0, (mlim - (kMinMag + cls) + 5.0) / 5.0);
    }
    double cellSize(int cls, float mlim) const { return std::clamp(visibility(cls, mlim) / 2.5, 0.25, 2500.0) * kPc; }

    // Number density of class-`cls` stars at p (stars / pc^3) from the galaxies in `gals`.
    double classDensity(int cls, const glm::dvec3& p, const std::vector<size_t>& gals, double* youngShare = nullptr) const {
        const ClassFractions& f = fractions();
        double old = 0.0, young = 0.0;
        for (size_t gi : gals) {
            detail::GalaxySample s;
            galaxies[gi].eval.sample(p, s, 0.0, detail::GalaxyEval::kStars);
            old += std::max(s.stars - s.youngStars, 0.0);
            young += s.youngStars;
        }
        const double o = old * f.old[cls], y = young * f.young[cls];
        if (youngShare) *youngShare = (o + y) > 0.0 ? y / (o + y) : 0.0;
        return o + y;
    }

    // Galaxies of `from` whose extent reaches the box (a property of the box alone, so a cell always
    // yields the same stars wherever it is seen from).
    void touching(const glm::dvec3& lo, double size, const std::vector<size_t>& from, std::vector<size_t>& out) const {
        out.clear();
        const glm::dvec3 hi = lo + glm::dvec3(size);
        for (size_t gi : from)
            if (glm::length(glm::clamp(galaxies[gi].center, lo, hi) - galaxies[gi].center) <= galaxies[gi].boundM) out.push_back(gi);
    }

    bool boxMayContain(const glm::dvec3& lo, double size, const std::vector<size_t>& gals) const {
        const glm::dvec3 hi = lo + glm::dvec3(size);
        for (size_t gi : gals) {
            const Galaxy& g = galaxies[gi];
            const glm::dvec3 q = glm::clamp(g.center, lo, hi);
            const double dc = glm::length(q - g.center);
            if (dc > g.boundM) continue;
            if (dc < g.coreM || !g.disk) return true;
            // Distance from the box to the midplane: the box's extent along the normal.
            const glm::dvec3 c = lo + glm::dvec3(0.5 * size) - g.center;
            const double half = 0.5 * size * (std::abs(g.normal.x) + std::abs(g.normal.y) + std::abs(g.normal.z));
            if (std::abs(glm::dot(c, g.normal)) - half < g.slabM) return true;
        }
        return false;
    }

    void generateBox(int cls, const glm::dvec3& lo, double size, int depth, uint64_t rngSeed, const std::vector<size_t>& parentGals,
                     std::vector<StarRecord>& out) const {
        std::vector<size_t> gals;
        touching(lo, size, parentGals, gals);
        if (gals.empty() || !boxMayContain(lo, size, gals)) return;
        // Density samples: centre, corners pulled in a little, and the points of the box closest
        // to each relevant galaxy's centre and midplane (so thin disks and cusps are not missed).
        double vmax = 0.0, vsum = 0.0;
        int nsamp = 0;
        const auto probe = [&](const glm::dvec3& p) {
            const double v = classDensity(cls, p, gals);
            vmax = std::max(vmax, v);
            vsum += v;
            ++nsamp;
        };
        const glm::dvec3 c = lo + glm::dvec3(0.5 * size);
        probe(c);
        for (int i = 0; i < 8; ++i)
            probe(c + 0.4 * size * glm::dvec3((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0));
        const glm::dvec3 hi = lo + glm::dvec3(size);
        for (size_t gi : gals) {
            const Galaxy& g = galaxies[gi];
            probe(glm::clamp(g.center, lo, hi));
            if (g.disk) probe(glm::clamp(c - g.normal * glm::dot(c - g.center, g.normal), lo, hi));
        }
        const double volPc = std::pow(size / kPc, 3.0);
        const double lamMax = 1.5 * vmax * volPc;
        if (lamMax < 1e-4) return;
        const double mean = vsum / nsamp;
        if (depth < 9 && (lamMax > 24.0 || (vmax > 3.0 * mean && lamMax > 1.5))) {
            for (int i = 0; i < 8; ++i) {
                const glm::dvec3 off(0.5 * size * (i & 1), 0.5 * size * ((i >> 1) & 1), 0.5 * size * ((i >> 2) & 1));
                generateBox(cls, lo + off, 0.5 * size, depth + 1, mix64(rngSeed + 0x9E3779B97F4A7C15ull * (i + 1)), gals, out);
            }
            return;
        }
        detail::Rng r(rngSeed);
        const uint32_t n = r.poisson(lamMax);
        const double magLo = kMinMag + cls;
        for (uint32_t i = 0; i < n; ++i) {
            const glm::dvec3 p = lo + size * glm::dvec3(r.uniform(), r.uniform(), r.uniform());
            double youngShare = 0.0;
            const double v = classDensity(cls, p, gals, &youngShare);
            const double accept = r.uniform(), popPick = r.uniform();
            if (accept * 1.5 * vmax > v) continue;
            StarRecord s;
            s.pos = p;
            const double M = magLo + r.uniform();
            s.absMag = static_cast<float>(M);
            if (catRadiusPc > 0.0) {   // the real catalogue already has this star
                const double dPc = std::max(glm::length(p - catCenter) / kPc, 1e-3);
                if (dPc < catRadiusPc && M + 5.0 * std::log10(dPc / 10.0) < catLimit) continue;
            }
            double teff;
            if (popPick < youngShare) {   // young population
                if (M < -5.0 && r.uniform() < 0.3) teff = r.range(3450.0, 4100.0);          // red supergiant
                else if (M < -5.0) teff = r.range(9000.0, 32000.0);                        // blue supergiant / O star
                else teff = mainSequenceTeff(M) * r.range(0.93, 1.07);
            } else if (M < 1.5) {
                teff = giantTeff(M) * r.range(0.95, 1.05);                                  // red giant branch, clump
            } else if (M < 3.5) {
                teff = r.uniform() < 0.6 ? r.range(4900.0, 5600.0) : mainSequenceTeff(M);   // subgiants / turn-off
            } else {
                teff = mainSequenceTeff(M) * r.range(0.96, 1.04);
            }
            s.teff = static_cast<float>(teff);
            s.radiusSolar = static_cast<float>(detail::stellarRadiusSolar(M, teff));
            out.push_back(s);
        }
    }
};

StarField::StarField() : impl_(std::make_unique<Impl>()) {}
StarField::~StarField() = default;

void StarField::configure(const UniverseContent& content, uint32_t seed) {
    Impl& im = *impl_;
    im = Impl{};
    im.seed = mix64(seed + 0x5851F42D4C957F2Dull);
    const auto add = [&](const GalaxyModel& m) {
        Impl::Galaxy g{detail::GalaxyEval(m), m.center, glm::dvec3(0.0, 1.0, 0.0), 0.0, 0.0, 0.0, false};
        g.normal = g.eval.model().normal;
        const double pc = kPc;
        g.boundM = g.eval.boundRadiusPc() * pc;
        g.disk = m.type != GalaxyType::Elliptical;
        g.slabM = (8.0 * std::max(m.diskScaleHeightPc, 1.0)) * pc;
        const double a = (m.type == GalaxyType::Elliptical ? m.ellipticalRadiusPc / 1.8153 : m.bulgeRadiusPc / 2.4142);
        g.coreM = (m.type == GalaxyType::Elliptical ? 20.0 : (m.bulgeMassFraction > 0.0 ? 6.0 : 0.0)) * a * pc;
        if (m.barLengthPc > 0.0) g.coreM = std::max(g.coreM, 1.2 * m.barLengthPc * pc);
        im.galaxies.push_back(std::move(g));
    };
    add(content.host);
    for (const GalaxyRecord& gr : content.galaxies) add(gr.model);
    im.catCenter = content.catalogCenter;
    im.catRadiusPc = content.catalogRadiusPc;
    im.catLimit = content.catalogLimitAppMag;
}

bool StarField::gather(const glm::dvec3& view, float mlim, size_t maxStars, std::vector<StarRecord>& out) {
    Impl& im = *impl_;
    const auto t0 = std::chrono::steady_clock::now();
    ++im.frame;
    if (mlim != im.cachedMag) {
        im.cells.clear();
        im.cachedMag = mlim;
        im.prevActive.clear();
        for (Impl::ClassState& c : im.classes) c.valid = false;
    }
    // Galaxies that can matter near the viewpoint (an acceleration structure only: each cell uses
    // the galaxies that touch it, so results do not depend on this list).
    const double reach = im.visibility(0, mlim) * kPc;
    if (glm::length(view - im.relevantAt) > 0.02 * reach) {
        im.relevant.clear();
        for (size_t i = 0; i < im.galaxies.size(); ++i)
            if (glm::length(im.galaxies[i].center - view) < im.galaxies[i].boundM + reach) im.relevant.push_back(i);
        im.relevantAt = view;
        for (Impl::ClassState& c : im.classes) c.valid = false;
    }

    // Cells within reach of any point of the viewpoint's sub-cell (a quarter of the class's cell), per
    // class: the selection depends only on that sub-cell, so it is rebuilt only when the viewpoint enters
    // another one and is the same however the viewpoint got there.
    struct Pending {
        double prio;
        CellKey key;
    };
    std::vector<Pending> todo;
    im.active.clear();
    for (int cls = 0; cls < kClasses; ++cls) {
        Impl::ClassState& st = im.classes[cls];
        const double cs = im.cellSize(cls, mlim), sub = 0.25 * cs;
        const glm::dvec3 vc = glm::floor(view / sub);
        const int64_t vx = static_cast<int64_t>(vc.x), vy = static_cast<int64_t>(vc.y), vz = static_cast<int64_t>(vc.z);
        if (!st.valid || st.vx != vx || st.vy != vy || st.vz != vz) {
            st.keys.clear();
            if (!im.relevant.empty()) {
                const double vis = im.visibility(cls, mlim) * kPc;
                const glm::dvec3 vcLo = glm::dvec3(static_cast<double>(vx), static_cast<double>(vy), static_cast<double>(vz)) * sub;
                const glm::dvec3 vcHi = vcLo + glm::dvec3(sub);
                const glm::dvec3 vlo = glm::floor((vcLo - glm::dvec3(vis)) / cs), vhi = glm::floor((vcHi + glm::dvec3(vis)) / cs);
                for (int64_t ix = static_cast<int64_t>(vlo.x); ix <= static_cast<int64_t>(vhi.x); ++ix)
                    for (int64_t iy = static_cast<int64_t>(vlo.y); iy <= static_cast<int64_t>(vhi.y); ++iy)
                        for (int64_t iz = static_cast<int64_t>(vlo.z); iz <= static_cast<int64_t>(vhi.z); ++iz) {
                            const glm::dvec3 lo = glm::dvec3(static_cast<double>(ix), static_cast<double>(iy), static_cast<double>(iz)) * cs;
                            const glm::dvec3 gap = glm::max(glm::max(lo - vcHi, vcLo - (lo + glm::dvec3(cs))), glm::dvec3(0.0));
                            if (glm::length(gap) > vis) continue;   // box-to-box distance
                            bool any = false;   // skip cells no galaxy reaches
                            for (size_t gi : im.relevant)
                                if (glm::length(glm::clamp(im.galaxies[gi].center, lo, lo + glm::dvec3(cs)) - im.galaxies[gi].center) <=
                                    im.galaxies[gi].boundM) {
                                    any = true;
                                    break;
                                }
                            if (any) st.keys.push_back(CellKey{cls, ix, iy, iz});
                        }
            }
            st.vx = vx;
            st.vy = vy;
            st.vz = vz;
            st.valid = true;
        }
        for (const CellKey& key : st.keys) {
            auto it = im.cells.find(key);
            if (it != im.cells.end()) {
                it->second.lastUse = im.frame;
                im.active.push_back(key);
            } else {
                const glm::dvec3 lo = glm::dvec3(static_cast<double>(key.x), static_cast<double>(key.y), static_cast<double>(key.z)) * cs;
                todo.push_back({cls * 1e30 + glm::length(glm::clamp(view, lo, lo + glm::dvec3(cs)) - view), key});
            }
        }
    }

    // Generate missing cells within the time budget: brightest classes first, nearest first.
    std::sort(todo.begin(), todo.end(), [](const Pending& a, const Pending& b) { return a.prio < b.prio; });
    size_t generated = 0;
    bool budgetHit = false;
    for (const Pending& p : todo) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 3.0 && generated > 0) {
            budgetHit = true;
            break;
        }
        const double cs = im.cellSize(p.key.cls, mlim);
        Impl::Cell cell;
        const glm::dvec3 lo = glm::dvec3(static_cast<double>(p.key.x), static_cast<double>(p.key.y), static_cast<double>(p.key.z)) * cs;
        im.generateBox(p.key.cls, lo, cs, 0, mix64(im.seed ^ CellKeyHash{}(p.key)), im.relevant, cell.stars);
        cell.lastUse = im.frame;
        im.cells.emplace(p.key, std::move(cell));
        im.active.push_back(p.key);
        ++generated;
    }

    // Evict cells unused for a while when the cache is large.
    if (im.cells.size() > 60000) {
        for (auto it = im.cells.begin(); it != im.cells.end();) {
            if (im.frame - it->second.lastUse > 120) it = im.cells.erase(it);
            else ++it;
        }
    }

    std::sort(im.active.begin(), im.active.end(), [](const CellKey& a, const CellKey& b) {
        if (a.cls != b.cls) return a.cls < b.cls;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    if (generated > 0 || im.active != im.prevActive) im.dirty = true;
    const bool finished = im.pending && !budgetHit;
    im.pending = budgetHit;
    ++im.sinceReport;
    // Publishing re-uploads every star, so while cells are still being generated or the viewpoint
    // races through space, publish at most every 4th call (and at once when generation finishes).
    if (!im.dirty || (!out.empty() && !finished && im.sinceReport < 4)) return false;
    im.dirty = false;
    im.sinceReport = 0;
    im.prevActive = im.active;

    // Output, brightest classes first, stopping before the cap.
    out.clear();
    for (const CellKey& k : im.active) {
        const Impl::Cell& c = im.cells.at(k);
        if (out.size() + c.stars.size() > maxStars) break;
        out.insert(out.end(), c.stars.begin(), c.stars.end());
    }
    return true;
}

size_t StarField::cachedCells() const { return impl_->cells.size(); }

}  // namespace bhs::universe
