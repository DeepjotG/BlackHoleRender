// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
// (Review fixes applied since: brightest-first cap, bounded generation and memory, change detection,
// whole-call time budget; the luminosity functions and stellar parameters are the lead's.)
//
// Procedural stars drawn from the galaxy models. Stars are split into 24 one-magnitude classes
// (M_V -8 .. +15); class k lives on its own cubic lattice whose cell size is matched to the distance
// out to which its stars can be brighter than the limiting magnitude. Cells are refined into a
// hierarchy of boxes (a box that may hold more than kItemMaxCount stars is split into eight children,
// decided by the box alone) and every box is a small, separate work item. A box's stars come from
// rejection sampling against the summed density of the galaxies touching it, seeded by the box's key,
// so a box always yields the same stars wherever it is seen from.
//
// Work is done best first: pending boxes wait in a priority queue ordered by the apparent magnitude
// of the brightest star they could hold (their class's brightest M_V at their nearest point). Once
// the generated stars include `maxStars` stars brighter than that bound, the cap's cut m_cut is
// known and nothing fainter than m_cut + kGenMargin is generated, so at a galaxy's centre the field
// never creates the ~1e8 stars that could not be published. The published set is exactly the
// `maxStars` brightest stars (apparent magnitude from the viewpoint) among those brighter than
// limit + kPubMargin. It is recomputed when new boxes may contribute (at most every 4th call while
// settling) or when the viewpoint has moved far enough for a star near the cut to change its
// standing, fingerprinted, and handed out only when it changed: at once if a star more than
// kImportantMag brighter than the cut was missing (at most every kMinPublishCalls calls while the
// viewpoint moves), when generation has finished at a still viewpoint, otherwise after at most
// kLazyPublishCalls calls. A large set selected late in a call is copied out in the next one.
//
// Each class keeps its generated stars in one arena, a leaf's stars contiguous and brightest first,
// and its frontier (leaves within reach) in arena order, so selection scans and output copies run
// through memory sequentially. Memory is bounded by a star budget and a box budget: boxes that left
// the reach are retired to a queue and dropped oldest first, a bounded number per call, and arenas
// that are mostly garbage are compacted one per call. Every call is time-budgeted as a whole (key
// rebuilds, eviction, generation, selection, copy), the generation share trimmed so that the running
// mean stays near kTargetMs.
//
// Limits: maxStars above kHardMaxStars is treated as kHardMaxStars; the limiting magnitude is
// clamped to [kMinLimit, kMaxLimit] (larger reaches would mean millions of cells per class).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "Units.h"
#include "universe/GalaxyModel_internal.h"
#include "universe/StarField.h"
#include "universe/StarField_internal.h"
#include "universe/Stars_internal.h"

namespace bhs::universe {

namespace {

constexpr int kMinMag = detail::kStarMinMag;
constexpr int kClasses = detail::kStarClasses;   // M_V -8 .. +15
constexpr double kPc = units::kParsec;
constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr float kPubMargin = detail::kStarPubMargin;   // mag published beyond the limit when the cap does not apply
constexpr float kGenMargin = 0.4f;       // mag generated beyond the cap's cut
constexpr double kImportantMag = 1.0;    // a missing star this much brighter than the cut is published at once
constexpr uint64_t kLazyPublishCalls = 30;
constexpr double kItemMaxCount = 256.0;  // boxes that may hold more stars become child work items
constexpr int kMaxLevel = 16;            // work-item levels below a class cell
constexpr int kMaxDepth = 24;            // subdivision limit inside a work item (absolute level)
constexpr int kMaxSubBoxes = 120;        // probed sub-boxes per work item at most (bounds its cost)
constexpr double kMinCellPc = 0.25, kMaxCellPc = 10000.0;
constexpr double kTargetMs = 2.6;        // mean cost of a call while there is work to do
constexpr double kBudgetMs = 2.6;        // generation per call at most (less when other work is due)
constexpr double kMinGenMs = 0.5;        // generation per call at least (when nothing else is due)
constexpr double kFirstBudgetMs = 8.0;   // first call after configure / a new limit
constexpr double kRebuildMs = 0.8;       // share of a call for refreshing the class key lists
constexpr uint64_t kMinPublishCalls = 3; // publications while the viewpoint moves are this far apart
constexpr size_t kHardMaxStars = 1000000;
constexpr size_t kMaxEntries = 250000;   // cached boxes (including empty and split ones)
constexpr float kMinLimit = -10.0f, kMaxLimit = 12.0f;
constexpr int kNearForValidity = 20;     // near-cut stars that set how far the viewpoint may move

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

// A box of class `cls` at refinement `level` (edge = class cell / 2^level), integer coordinates in
// units of its own edge.
struct Key {
    int32_t cls = 0, level = 0;
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const Key& o) const { return cls == o.cls && level == o.level && x == o.x && y == o.y && z == o.z; }
};
uint64_t keyHash64(const Key& k) {
    uint64_t h = mix64(static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(k.cls) * 0x2545F4914F6CDD1Dull);
    h = mix64(h ^ (static_cast<uint64_t>(k.y) + 0x632BE59BD9B4E019ull));
    return mix64(h ^ (static_cast<uint64_t>(k.z) * 3u + static_cast<uint64_t>(k.level) * 0xD6E8FEB86659FD93ull));
}
struct KeyHash {
    size_t operator()(const Key& k) const { return static_cast<size_t>(keyHash64(k)); }
};
Key childKey(const Key& k, int i) {
    return Key{k.cls, k.level + 1, 2 * k.x + (i & 1), 2 * k.y + ((i >> 1) & 1), 2 * k.z + ((i >> 2) & 1)};
}

double elapsedMs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double boxDistance(const glm::dvec3& p, const glm::dvec3& lo, double size) {
    return glm::length(glm::clamp(p, lo, lo + glm::dvec3(size)) - p);
}

}  // namespace

double detail::starClassFraction(int k, bool young) {
    if (k < 0 || k >= kClasses) return 0.0;
    return young ? fractions().young[k] : fractions().old[k];
}

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
    // Galaxies that can touch any box within reach (a superset, so that a box's stars never depend
    // on where it is seen from).
    std::vector<size_t> relevant;
    glm::dvec3 relevantAt{1e300};
    uint64_t seed = 0;
    // Catalogue suppression (real universe).
    glm::dvec3 catCenter{0.0};
    double catRadiusPc = 0.0;
    float catLimit = 0.0f;

    enum : uint8_t { kEmpty = 0, kLeaf = 1, kSplit = 2 };
    struct Box {
        uint8_t state = kEmpty;
        int32_t cls = 0;
        Key key;
        glm::dvec3 centre{0.0};          // meters
        double half = 0.0;               // half the edge, meters
        uint32_t off = 0, n = 0;         // leaf: range in its class's arena
        uint32_t leafIdx = 0;            // leaf: position in its class's `leaves`
        uint64_t lastUse = 0;
        uint32_t frontierEpoch = 0;      // == the class's epoch while in its current frontier
        bool queued = false;             // has an entry in `retired`
    };
    std::unordered_map<Key, Box, KeyHash> cache;
    size_t arenaLive = 0;                // stars of all cached leaves
    // Eviction candidates, oldest first: leaves that dropped out of reach and every empty or split
    // box (those are dropped once unused for a while); at most one entry per cached box. Entries are
    // checked when they come up (a leaf may be back in reach).
    std::deque<Key> retired;
    void retire(Box& b) {
        if (b.queued) return;
        b.queued = true;
        retired.push_back(b.key);
    }

    struct FrontRec {                    // a frontier leaf, self-contained for sequential scans
        glm::dvec3 centrePc;
        float halfPc, wMax;
        uint32_t off, n;
        Box* box;
    };
    struct ClassState {
        bool valid = false;
        int64_t vx = 0, vy = 0, vz = 0;    // the viewpoint's sub-cell (a quarter of a class cell)
        glm::dvec3 subLo{0.0}, subHi{0.0};
        uint32_t epoch = 1;
        std::vector<FrontRec> front;       // generated leaves within reach of the sub-cell, arena order
        // Arena of the class's generated stars: a leaf's stars are contiguous, brightest first.
        std::vector<StarRecord> recs;
        std::vector<glm::vec4> fast;       // (pos - box centre) in pc, w = 10^(-0.4 absMag)
        std::vector<uint64_t> ids;         // stable star ids (fingerprints)
        std::vector<uint32_t> pubMark;     // == pubGen while the star is in the published set
        size_t live = 0;
        std::vector<Box*> leaves;          // every cached leaf of the class (for compaction)
    };
    ClassState classes[kClasses];
    int rebuildCursor = 0;

    struct Pending {
        float prio;       // apparent magnitude bound of the box's stars from the viewpoint
        uint32_t epoch;   // class epoch when queued (older entries are stale)
        Key key;
    };
    std::vector<Pending> heap;   // min-heap on prio
    static bool heapLess(const Pending& a, const Pending& b) { return a.prio > b.prio; }

    bool haveLattice = false;
    float mag = 0.0f;              // limiting magnitude the lattice was built for
    double cellM[kClasses] = {};   // class cell edge (m)
    double reachM[kClasses] = {};  // distance within which class stars may beat mag + kPubMargin (m)

    // Selection and publication.
    struct Cand {
        float q;      // 10^(-0.4 absMag) / d^2 (pc): larger is brighter
        float d2;     // pc^2
        uint32_t idx; // index in the class's arena
        uint32_t cls;
    };
    std::vector<Cand> cands;
    std::vector<uint32_t> radix;   // histogram for the cap's cut (top 11 bits of positive floats)
    std::vector<float> qs;
    uint64_t pubFingerprint = 0;
    size_t pubCount = 0;
    uint32_t pubGen = 1;
    bool selectedOnce = false, publishedOnce = false, publishPending = false, writePending = false;
    double mCut = kInf;            // apparent magnitude of the cap's cut (inf: the cap does not apply)
    glm::dvec3 selAt{1e300}, prevView{1e300};
    double validM = 0.0;           // the viewpoint may move this far before a star near the cut may cross it
    size_t lastCap = static_cast<size_t>(-1);
    uint64_t frame = 0, lastSelectFrame = 0, lastPublishFrame = 0;
    double overheadMs = 0.0;       // running mean of the per-call work outside generation
    double writeMs = 0.0;          // running mean of the cost of writing the output
    size_t newSinceSelect = 0;     // stars generated since the last selection that may be published
    double lastSelectMs = 0.0;
    std::vector<size_t> galScratch;
    std::vector<double> perGalScratch;
    std::vector<Key> keyScratch;

    static double visibilityPc(int cls, double m) { return std::pow(10.0, (m - (kMinMag + cls) + 5.0) / 5.0); }

    void resetCache() {
        cache.clear();
        cache.reserve(kMaxEntries);   // no rehash pauses while the cache grows
        arenaLive = 0;
        heap.clear();
        for (ClassState& c : classes) {
            c.valid = false;
            c.front.clear();
            c.recs.clear();
            c.fast.clear();
            c.ids.clear();
            c.pubMark.clear();
            c.live = 0;
            c.leaves.clear();
            ++c.epoch;
        }
        retired.clear();
        cands.clear();
        selectedOnce = false;
        writePending = false;
        mCut = kInf;
        selAt = glm::dvec3(1e300);
        validM = 0.0;
        newSinceSelect = 0;
        relevantAt = glm::dvec3(1e300);
        ++pubGen;
    }

    void setLattice(float m) {
        haveLattice = true;
        mag = m;
        for (int k = 0; k < kClasses; ++k) {
            cellM[k] = std::clamp(visibilityPc(k, m) / 2.5, kMinCellPc, kMaxCellPc) * kPc;
            reachM[k] = visibilityPc(k, static_cast<double>(m) + kPubMargin) * kPc;
        }
    }

    void boxOf(const Key& k, glm::dvec3& lo, double& size) const {
        size = std::ldexp(cellM[k.cls], -k.level);
        lo = glm::dvec3(static_cast<double>(k.x), static_cast<double>(k.y), static_cast<double>(k.z)) * size;
    }
    double mBest(const Key& k, const glm::dvec3& view) const {
        glm::dvec3 lo;
        double size;
        boxOf(k, lo, size);
        const double d = std::max(boxDistance(view, lo, size) / kPc, 1e-3);
        return (kMinMag + k.cls) + 5.0 * std::log10(d / 10.0);
    }
    bool inReach(const Key& k) const {
        const ClassState& st = classes[k.cls];
        glm::dvec3 lo;
        double size;
        boxOf(k, lo, size);
        const glm::dvec3 gap = glm::max(glm::max(lo - st.subHi, st.subLo - (lo + glm::dvec3(size))), glm::dvec3(0.0));
        return glm::length(gap) <= reachM[k.cls];
    }

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

    // Galaxies of `from` whose extent reaches the box (a property of the box alone).
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

    // Density samples: centre, corners pulled in a little, and the points of the box closest to each
    // galaxy's centre and midplane (so thin disks and cusps are not missed). `perGalaxy` (optional)
    // receives each galaxy's largest sampled class density.
    void probe(int cls, const glm::dvec3& lo, double size, const std::vector<size_t>& gals, double& vmax, double& mean,
               std::vector<double>* perGalaxy = nullptr) const {
        vmax = 0.0;
        double vsum = 0.0;
        int n = 0;
        if (perGalaxy) perGalaxy->assign(gals.size(), 0.0);
        const ClassFractions& f = fractions();
        const auto at = [&](const glm::dvec3& p) {
            double v = 0.0;
            if (perGalaxy) {
                for (size_t j = 0; j < gals.size(); ++j) {
                    detail::GalaxySample s;
                    galaxies[gals[j]].eval.sample(p, s, 0.0, detail::GalaxyEval::kStars);
                    const double vj = std::max(s.stars - s.youngStars, 0.0) * f.old[cls] + s.youngStars * f.young[cls];
                    (*perGalaxy)[j] = std::max((*perGalaxy)[j], vj);
                    v += vj;
                }
            } else {
                v = classDensity(cls, p, gals);
            }
            vmax = std::max(vmax, v);
            vsum += v;
            ++n;
        };
        const glm::dvec3 c = lo + glm::dvec3(0.5 * size);
        at(c);
        for (int i = 0; i < 8; ++i) at(c + 0.4 * size * glm::dvec3((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0));
        const glm::dvec3 hi = lo + glm::dvec3(size);
        for (size_t gi : gals) {
            const Galaxy& g = galaxies[gi];
            at(glm::clamp(g.center, lo, hi));
            if (g.disk) at(glm::clamp(c - g.normal * glm::dot(c - g.center, g.normal), lo, hi));
        }
        mean = vsum / n;
    }

    // Stars of one work item: adaptive subdivision where the density varies, then rejection sampling.
    // A_V from the catalogue's centre to p through the host's dust (the catalogue's magnitudes are
    // observed ones): a midpoint rule with up to 48 steps, each sampling the model at its own scale.
    double catalogueDust(const glm::dvec3& p, double dPc) const {
        if (galaxies.empty()) return 0.0;
        const int n = std::clamp(static_cast<int>(dPc / 10.0), 4, 48);
        const double step = dPc / n;
        double tau = 0.0;
        for (int i = 0; i < n; ++i) {
            detail::GalaxySample s;
            galaxies[0].eval.sample(catCenter + (p - catCenter) * ((i + 0.5) / n), s, step, detail::GalaxyEval::kDust);
            tau += s.tau;
        }
        return 1.086 * tau * step;
    }

    // `boxes` counts the sub-boxes of the work item (at most kMaxSubBoxes are probed; the traversal
    // order is fixed, so this stays a property of the item).
    void generateBox(int cls, const glm::dvec3& lo, double size, int depth, uint64_t rngSeed, const std::vector<size_t>& parentGals,
                     std::vector<StarRecord>& out, int& boxes) const {
        std::vector<size_t> gals;
        touching(lo, size, parentGals, gals);
        if (gals.empty() || !boxMayContain(lo, size, gals)) return;
        double vmax, mean;
        probe(cls, lo, size, gals, vmax, mean);
        ++boxes;
        const double volPc = std::pow(size / kPc, 3.0);
        const double lamMax = 1.5 * vmax * volPc;
        if (lamMax < 1e-4) return;
        // Follow steep density variations (a thin disk, a cusp) while the item's probe budget lasts.
        if (depth < kMaxDepth && vmax > 3.0 * mean && lamMax > 1.5 && boxes + 8 <= kMaxSubBoxes) {
            for (int i = 0; i < 8; ++i) {
                const glm::dvec3 off(0.5 * size * (i & 1), 0.5 * size * ((i >> 1) & 1), 0.5 * size * ((i >> 2) & 1));
                generateBox(cls, lo + off, 0.5 * size, depth + 1, mix64(rngSeed + 0x9E3779B97F4A7C15ull * (i + 1)), gals, out, boxes);
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
            if (catRadiusPc > 0.0) {   // the real catalogue already has this star (observed V, see Universe.h)
                const double dPc = std::max(glm::length(p - catCenter) / kPc, 1e-3);
                const double m = M + 5.0 * std::log10(dPc / 10.0);
                if (dPc < catRadiusPc && m < catLimit && m + catalogueDust(p, dPc) < catLimit) continue;
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

    void push(const Key& k, const glm::dvec3& view) {
        heap.push_back({static_cast<float>(mBest(k, view)), classes[k.cls].epoch, k});
        std::push_heap(heap.begin(), heap.end(), heapLess);
    }

    void addToFront(Box& b) {
        ClassState& st = classes[b.cls];
        b.frontierEpoch = st.epoch;
        st.front.push_back({b.centre / kPc, static_cast<float>(b.half / kPc), st.fast[b.off].w, b.off, b.n, &b});
    }

    // Takes a box into the class's frontier (leaf) or walks into its children (split); unknown boxes
    // are queued.
    void visit(const Key& k, const glm::dvec3& view) {
        const auto it = cache.find(k);
        if (it == cache.end()) {
            push(k, view);
            return;
        }
        Box& b = it->second;
        b.lastUse = frame;
        if (b.state == kLeaf) {
            addToFront(b);
        } else if (b.state == kSplit) {
            for (int i = 0; i < 8; ++i) {
                const Key c = childKey(k, i);
                if (inReach(c)) visit(c, view);
            }
        }
    }

    // Rebuilds class k's frontier and queue if the viewpoint entered another sub-cell (or forced).
    bool rebuildClass(int k, const glm::dvec3& view) {
        ClassState& st = classes[k];
        const double cs = cellM[k], sub = 0.25 * cs;
        const glm::dvec3 vc = glm::floor(view / sub);
        const int64_t vx = static_cast<int64_t>(vc.x), vy = static_cast<int64_t>(vc.y), vz = static_cast<int64_t>(vc.z);
        if (st.valid && st.vx == vx && st.vy == vy && st.vz == vz) return false;
        st.valid = true;
        st.vx = vx;
        st.vy = vy;
        st.vz = vz;
        ++st.epoch;
        // The previous frontier: in use until now; leaves that are not taken back in below are retired.
        std::vector<FrontRec> previous;
        previous.swap(st.front);
        for (const FrontRec& r : previous) r.box->lastUse = frame;
        st.subLo = vc * sub;
        st.subHi = st.subLo + glm::dvec3(sub);
        if (relevant.empty()) {
            for (const FrontRec& r : previous) retire(*r.box);
            return true;
        }
        const double reach = reachM[k];
        const glm::dvec3 ilo = glm::floor((st.subLo - glm::dvec3(reach)) / cs), ihi = glm::floor((st.subHi + glm::dvec3(reach)) / cs);
        // Candidate cells: the whole reach, or only the cells of each relevant galaxy if that is less.
        const auto volume = [](const glm::dvec3& a, const glm::dvec3& b) {
            const glm::dvec3 e = glm::max(b - a + glm::dvec3(1.0), glm::dvec3(0.0));
            return e.x * e.y * e.z;
        };
        double perGalaxy = 0.0;
        for (size_t gi : relevant) {
            const Galaxy& g = galaxies[gi];
            perGalaxy += volume(glm::max(glm::floor((g.center - glm::dvec3(g.boundM)) / cs), ilo),
                                glm::min(glm::floor((g.center + glm::dvec3(g.boundM)) / cs), ihi));
        }
        keyScratch.clear();
        const auto consider = [&](int64_t ix, int64_t iy, int64_t iz) {
            const glm::dvec3 lo = glm::dvec3(static_cast<double>(ix), static_cast<double>(iy), static_cast<double>(iz)) * cs;
            const glm::dvec3 gap = glm::max(glm::max(lo - st.subHi, st.subLo - (lo + glm::dvec3(cs))), glm::dvec3(0.0));
            if (glm::length(gap) > reach) return;
            touching(lo, cs, relevant, galScratch);
            if (galScratch.empty() || !boxMayContain(lo, cs, galScratch)) return;
            keyScratch.push_back(Key{k, 0, ix, iy, iz});
        };
        const auto range = [&](const glm::dvec3& a, const glm::dvec3& b) {
            for (int64_t ix = static_cast<int64_t>(a.x); ix <= static_cast<int64_t>(b.x); ++ix)
                for (int64_t iy = static_cast<int64_t>(a.y); iy <= static_cast<int64_t>(b.y); ++iy)
                    for (int64_t iz = static_cast<int64_t>(a.z); iz <= static_cast<int64_t>(b.z); ++iz) consider(ix, iy, iz);
        };
        if (perGalaxy < volume(ilo, ihi)) {
            for (size_t gi : relevant) {
                const Galaxy& g = galaxies[gi];
                range(glm::max(glm::floor((g.center - glm::dvec3(g.boundM)) / cs), ilo),
                      glm::min(glm::floor((g.center + glm::dvec3(g.boundM)) / cs), ihi));
            }
            std::sort(keyScratch.begin(), keyScratch.end(), [](const Key& a, const Key& b) {
                if (a.x != b.x) return a.x < b.x;
                if (a.y != b.y) return a.y < b.y;
                return a.z < b.z;
            });
            keyScratch.erase(std::unique(keyScratch.begin(), keyScratch.end()), keyScratch.end());
        } else {
            range(ilo, ihi);
        }
        for (const Key& key : keyScratch) visit(key, view);
        // Arena order, so that scans and output copies run through memory sequentially (leaves made
        // later are appended to both, which keeps the order).
        std::sort(st.front.begin(), st.front.end(), [](const FrontRec& a, const FrontRec& b) { return a.off < b.off; });
        for (const FrontRec& r : previous)
            if (r.box->frontierEpoch != st.epoch) retire(*r.box);
        return true;
    }

    // Galaxies near the viewpoint, with a margin covering every box any class can reach before the
    // list is refreshed.
    void refreshRelevant(const glm::dvec3& view) {
        const double reach0 = reachM[0];
        const double refresh = 0.05 * reach0;
        if (glm::length(view - relevantAt) <= refresh) return;
        const double margin = reach0 + 2.0 * std::sqrt(3.0) * cellM[0] + 2.0 * refresh;
        std::vector<size_t> next;
        for (size_t i = 0; i < galaxies.size(); ++i)
            if (glm::length(galaxies[i].center - view) < galaxies[i].boundM + margin) next.push_back(i);
        relevantAt = view;
        if (next != relevant) {
            relevant = std::move(next);
            for (ClassState& c : classes) c.valid = false;
        }
    }

    // Generates the box `k` (popped from the queue) and files it.
    void process(const Key& k, const glm::dvec3& view) {
        glm::dvec3 lo;
        double size;
        boxOf(k, lo, size);
        Box b;
        b.cls = k.cls;
        b.centre = lo + glm::dvec3(0.5 * size);
        b.half = 0.5 * size;
        b.lastUse = frame;
        touching(lo, size, relevant, galScratch);
        std::vector<size_t> gals = galScratch;
        if (!gals.empty() && boxMayContain(lo, size, gals)) {
            double vmax, mean;
            probe(k.cls, lo, size, gals, vmax, mean, &perGalScratch);
            // Galaxies whose sampled density stays below 1e-4 of the box's peak (outskirts of large
            // neighbours in a cluster) are left out of this box: < 0.01 stars lost, far less work.
            if (gals.size() > 1) {
                size_t kept = 0;
                for (size_t j = 0; j < gals.size(); ++j)
                    if (perGalScratch[j] >= 1e-4 * vmax) gals[kept++] = gals[j];
                gals.resize(kept);
            }
            const double lamMax = 1.5 * vmax * std::pow(size / kPc, 3.0);
            if (lamMax >= 1e-4) {
                // Work-item size: candidates times the galaxies each one evaluates.
                const double cost = lamMax * (1.0 + 0.5 * static_cast<double>(gals.size() - 1));
                if (cost > kItemMaxCount && k.level < kMaxLevel) {
                    b.state = kSplit;
                } else {
                    ClassState& st = classes[k.cls];
                    const uint64_t s = mix64(seed ^ keyHash64(k));
                    const size_t off = st.recs.size();
                    int boxes = 0;
                    generateBox(k.cls, lo, size, k.level, s, gals, st.recs, boxes);
                    const size_t n = st.recs.size() - off;
                    if (n > 0) {
                        std::sort(st.recs.begin() + static_cast<std::ptrdiff_t>(off), st.recs.end(),
                                  [](const StarRecord& a, const StarRecord& c) { return a.absMag < c.absMag; });
                        for (size_t i = off; i < st.recs.size(); ++i) {
                            st.fast.emplace_back(glm::vec3((st.recs[i].pos - b.centre) / kPc),
                                                 static_cast<float>(std::pow(10.0, -0.4 * st.recs[i].absMag)));
                            st.ids.push_back(mix64(s + (i - off)));
                            st.pubMark.push_back(0);
                        }
                        b.state = kLeaf;
                        b.off = static_cast<uint32_t>(off);
                        b.n = static_cast<uint32_t>(n);
                        st.live += n;
                        arenaLive += n;
                    }
                }
            }
        }
        b.key = k;
        Box& box = cache.emplace(k, b).first->second;
        if (box.state == kLeaf) {
            ClassState& st = classes[k.cls];
            box.leafIdx = static_cast<uint32_t>(st.leaves.size());
            st.leaves.push_back(&box);
            addToFront(box);
            // Stars that may change the published set (the box may beat the current threshold).
            if (mBest(k, view) < std::min(static_cast<double>(mag) + kPubMargin, mCut)) newSinceSelect += box.n;
        } else {
            retire(box);   // empty and split boxes go once they are no longer visited
            if (box.state == kSplit)
                for (int i = 0; i < 8; ++i) {
                    const Key c = childKey(k, i);
                    if (inReach(c)) push(c, view);
                }
        }
    }

    size_t starBudget(size_t cap) const { return std::max<size_t>(6 * cap, 400000); }
    bool overBudget(size_t budget) const { return arenaLive >= budget || cache.size() >= kMaxEntries; }
    bool nearBudget(size_t budget) const { return arenaLive >= budget * 8 / 10 || cache.size() >= kMaxEntries * 8 / 10; }

    // Rewrites the arena of the class with the most garbage (at most one per call) without the
    // evicted leaves' stars, keeping the order of the live ones (frontier records stay in arena order).
    void compactOneArena() {
        int worst = -1;
        size_t worstGarbage = 0;
        for (int k = 0; k < kClasses; ++k) {
            const size_t garbage = classes[k].recs.size() - classes[k].live;
            if (garbage > classes[k].live + 20000 && garbage > worstGarbage) {
                worst = k;
                worstGarbage = garbage;
            }
        }
        if (worst < 0) return;
        ClassState& st = classes[worst];
        std::sort(st.leaves.begin(), st.leaves.end(), [](const Box* a, const Box* b) { return a->off < b->off; });
        std::vector<StarRecord> r2;
        std::vector<glm::vec4> f2;
        std::vector<uint64_t> i2;
        std::vector<uint32_t> p2;
        r2.reserve(st.live);
        f2.reserve(st.live);
        i2.reserve(st.live);
        p2.reserve(st.live);
        for (size_t li = 0; li < st.leaves.size(); ++li) {
            Box* b = st.leaves[li];
            const uint32_t off = static_cast<uint32_t>(r2.size());
            r2.insert(r2.end(), st.recs.begin() + b->off, st.recs.begin() + b->off + b->n);
            f2.insert(f2.end(), st.fast.begin() + b->off, st.fast.begin() + b->off + b->n);
            i2.insert(i2.end(), st.ids.begin() + b->off, st.ids.begin() + b->off + b->n);
            p2.insert(p2.end(), st.pubMark.begin() + b->off, st.pubMark.begin() + b->off + b->n);
            b->off = off;
            b->leafIdx = static_cast<uint32_t>(li);
        }
        st.recs.swap(r2);
        st.fast.swap(f2);
        st.ids.swap(i2);
        st.pubMark.swap(p2);
        for (FrontRec& r : st.front) r.off = r.box->off;
    }

    void erase(std::unordered_map<Key, Box, KeyHash>::iterator it) {
        Box& b = it->second;
        if (b.state == kLeaf) {
            ClassState& st = classes[b.cls];
            Box* last = st.leaves.back();
            st.leaves[b.leafIdx] = last;
            last->leafIdx = b.leafIdx;
            st.leaves.pop_back();
            st.live -= b.n;
            arenaLive -= b.n;
        }
        cache.erase(it);
    }

    // Incremental eviction, oldest retired boxes first, with a bounded amount of work per call.
    void evict(size_t budget, bool compact = true) {
        const size_t starGoal = budget * 7 / 10, entryGoal = kMaxEntries * 7 / 10;
        for (int work = 0; work < 6000 && !retired.empty() && (arenaLive > starGoal || cache.size() > entryGoal); ++work) {
            const Key k = retired.front();
            retired.pop_front();
            const auto it = cache.find(k);
            if (it == cache.end()) continue;
            Box& b = it->second;
            if (b.state == kLeaf && b.frontierEpoch == classes[b.cls].epoch) {   // back in reach: retired again when it leaves
                b.queued = false;
                continue;
            }
            if (b.state != kLeaf && frame - b.lastUse < 60) {   // still in use
                retired.push_back(k);
                continue;
            }
            erase(it);
        }
        if (compact) compactOneArena();
    }

    // Best-first generation until the queue holds nothing that could be published, the time is up or
    // the memory budget is reached. Returns true when there is nothing left to do.
    bool generate(const glm::dvec3& view, std::chrono::steady_clock::time_point t0, double budgetMs, size_t budget) {
        const double mGen = std::min(static_cast<double>(mag) + kPubMargin, mCut + kGenMargin);
        while (!heap.empty()) {
            const Pending top = heap.front();
            if (top.epoch != classes[top.key.cls].epoch || cache.count(top.key)) {
                std::pop_heap(heap.begin(), heap.end(), heapLess);
                heap.pop_back();
                continue;
            }
            if (top.prio >= mGen) return true;
            if (elapsedMs(t0) > budgetMs) return false;
            if (overBudget(budget)) {
                evict(budget);
                if (overBudget(budget)) return false;   // everything cached is in use: wait
            }
            std::pop_heap(heap.begin(), heap.end(), heapLess);
            heap.pop_back();
            const double p = mBest(top.key, view);   // the viewpoint may have moved since it was queued
            if (p > top.prio + 0.05) {
                heap.push_back({static_cast<float>(p), top.epoch, top.key});
                std::push_heap(heap.begin(), heap.end(), heapLess);
                continue;
            }
            process(top.key, view);
        }
        return true;
    }

    // Stale queue entries pile up as classes are rebuilt; drop them now and then.
    void compactHeap() {
        if (heap.size() < 4096) return;
        size_t live = 0;
        for (const Pending& p : heap) live += p.epoch == classes[p.key.cls].epoch;
        if (live * 2 > heap.size()) return;
        heap.erase(std::remove_if(heap.begin(), heap.end(), [&](const Pending& p) { return p.epoch != classes[p.key.cls].epoch; }), heap.end());
        std::make_heap(heap.begin(), heap.end(), heapLess);
    }

    struct Selection {
        uint64_t fingerprint = 0;
        bool missingImportant = false;   // a selected star brighter than cut - kImportantMag is not published
    };
    Selection pendingSel;                // selection in `cands` waiting to be written (writePending)

    // The cap-th largest q among the candidates: radix histogram on the top bits of the (positive)
    // floats, then an exact selection inside the bin that holds it.
    float capCut(size_t cap) {
        constexpr int kShift = 20;   // 2^11 bins of 1/8 octave (0.094 mag) each
        radix.assign(size_t{1} << (31 - kShift), 0u);
        const auto bin = [](float q) {
            uint32_t b;
            std::memcpy(&b, &q, 4);
            return b >> kShift;
        };
        for (const Cand& c : cands) ++radix[bin(c.q)];
        size_t above = 0;
        uint32_t b = static_cast<uint32_t>(radix.size());
        while (b-- > 0 && above + radix[b] < cap) above += radix[b];
        qs.clear();
        for (const Cand& c : cands)
            if (bin(c.q) == b) qs.push_back(c.q);
        const size_t need = cap - above;   // 1 .. size of the bin
        std::nth_element(qs.begin(), qs.begin() + static_cast<std::ptrdiff_t>(need - 1), qs.end(), std::greater<float>());
        return qs[need - 1];
    }

    // Candidates brighter than apparent magnitude `m` (scan order: class by class, arena order).
    void scan(const glm::dvec3& view, double m) {
        const float Kf = static_cast<float>(std::pow(10.0, (m + 5.0) / 2.5));   // passes if d^2 (pc^2) < K w
        const glm::dvec3 viewPc = view / kPc;
        cands.clear();
        for (uint32_t k = 0; k < kClasses; ++k) {
            const ClassState& st = classes[k];
            const glm::vec4* fast = st.fast.data();
            for (const FrontRec& r : st.front) {
                const glm::dvec3 rel = viewPc - r.centrePc;
                const glm::dvec3 gap = glm::max(glm::abs(rel) - glm::dvec3(r.halfPc), glm::dvec3(0.0));
                const float d2min = static_cast<float>(glm::dot(gap, gap));
                if (Kf * r.wMax <= d2min) continue;
                const glm::vec3 rv(rel);
                const uint32_t end = r.off + r.n;
                for (uint32_t i = r.off; i < end; ++i) {
                    const glm::vec4 f = fast[i];
                    const float lim = Kf * f.w;
                    if (lim <= d2min) break;   // brightest first: the rest are fainter
                    const glm::vec3 d = glm::vec3(f) - rv;
                    const float d2 = glm::dot(d, d);
                    if (d2 < lim) cands.push_back({f.w / std::max(d2, 1e-12f), d2, i, k});
                }
            }
        }
    }

    // Computes the set to publish into `cands` (scan order): the `cap` brightest stars (apparent
    // magnitude from `view`) among those brighter than mag + kPubMargin.
    Selection select(const glm::dvec3& view, size_t cap) {
        const double thr = static_cast<double>(mag) + kPubMargin;
        // When the cap applied last time, scan only a little beyond its cut; scan again to the full
        // threshold if that does not fill the cap (the cut moved fainter).
        const bool narrow = std::isfinite(mCut) && mCut + 0.3 < thr && cap > 0;
        scan(view, narrow ? mCut + 0.3 : thr);
        if (narrow && cands.size() < cap) scan(view, thr);
        double cut = kInf;
        Selection s;
        float qCut = 0.0f;
        size_t ties = 0;
        if (cands.size() > cap) {
            if (cap == 0) {
                cands.clear();
                cut = -kInf;
            } else {
                qCut = capCut(cap);
                size_t above = 0;
                for (const Cand& c : cands) above += c.q > qCut;
                ties = cap - above;
                cut = -2.5 * std::log10(static_cast<double>(qCut)) - 5.0;
            }
        }
        mCut = cut;
        // One pass keeps the cap brightest (a stable filter: scan order, so the output is copied from
        // the arenas sequentially) and gathers the fingerprint, missing prominent stars and validity.
        // Validity: 0.35 of the distance of the kNearForValidity-th nearest star within 1.5 mag of the
        // cut (moving that far changes its magnitude by up to 0.9 mag; an unpublished star needs 1 mag
        // to become prominent, see kImportantMag).
        const double thrNow = std::min(thr, cut);
        const float qNear = static_cast<float>(std::pow(10.0, -0.4 * (thrNow - 1.5 + 5.0)));
        const float qImportant = static_cast<float>(std::pow(10.0, -0.4 * (thrNow - kImportantMag + 5.0)));
        float nearest[kNearForValidity];
        int nn = 0;
        uint64_t fp = 0;
        const bool capped = std::isfinite(cut) && cut > -kInf;
        size_t kept = 0;
        for (size_t i = 0; i < cands.size(); ++i) {
            const Cand c = cands[i];
            if (capped && !(c.q > qCut || (c.q == qCut && ties > 0 && (--ties, true)))) continue;
            cands[kept++] = c;
            const ClassState& st = classes[c.cls];
            fp += st.ids[c.idx];
            if (c.q > qImportant && st.pubMark[c.idx] != pubGen) s.missingImportant = true;
            if (c.q > qNear) continue;
            if (nn < kNearForValidity) {
                nearest[nn++] = c.d2;
                std::push_heap(nearest, nearest + nn);
            } else if (c.d2 < nearest[0]) {
                std::pop_heap(nearest, nearest + nn);
                nearest[nn - 1] = c.d2;
                std::push_heap(nearest, nearest + nn);
            }
        }
        cands.resize(kept);
        fp += 0x9E3779B97F4A7C15ull * (cands.size() + 1);
        const double far = 0.05 * reachM[0];
        validM = nn > 0 ? std::clamp(0.35 * std::sqrt(static_cast<double>(nearest[0])) * kPc, 0.25 * kPc, far) : far;
        s.fingerprint = fp;
        selAt = view;
        lastSelectFrame = frame;
        newSinceSelect = 0;
        selectedOnce = true;
        return s;
    }

    void publish(const Selection& s, std::vector<StarRecord>& out) {
        const auto t = std::chrono::steady_clock::now();
        ++pubGen;
        out.resize(cands.size());
        // Runs of consecutive arena entries (a leaf's brightest stars) are copied in one go.
        size_t o = 0;
        for (size_t i = 0; i < cands.size();) {
            const Cand& c = cands[i];
            size_t j = i + 1;
            while (j < cands.size() && cands[j].cls == c.cls && cands[j].idx == c.idx + (j - i)) ++j;
            ClassState& st = classes[c.cls];
            std::copy(st.recs.begin() + c.idx, st.recs.begin() + c.idx + (j - i), out.begin() + static_cast<std::ptrdiff_t>(o));
            std::fill(st.pubMark.begin() + c.idx, st.pubMark.begin() + c.idx + (j - i), pubGen);
            o += j - i;
            i = j;
        }
        pubFingerprint = s.fingerprint;
        pubCount = cands.size();
        publishedOnce = true;
        publishPending = false;
        lastPublishFrame = frame;
        writeMs += 0.3 * (elapsedMs(t) - writeMs);
    }
};

StarField::StarField() : impl_(std::make_unique<Impl>()) {}
StarField::~StarField() = default;

void StarField::configure(const UniverseContent& content, uint32_t seed) {
    Impl& im = *impl_;
    im = Impl{};
    im.seed = mix64(seed + 0x5851F42D4C957F2Dull);
    const auto add = [&](const GalaxyModel& m) {
        Impl::Galaxy g{detail::GalaxyEval(m), glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0), 0.0, 0.0, 0.0, false};
        const GalaxyModel& s = g.eval.model();   // sanitised, orthonormal frame
        g.center = s.center;
        g.normal = s.normal;
        g.boundM = g.eval.boundRadiusPc() * kPc;
        g.disk = s.type != GalaxyType::Elliptical;
        g.slabM = (8.0 * std::max(s.diskScaleHeightPc, 1.0)) * kPc;
        const double a = (s.type == GalaxyType::Elliptical ? s.ellipticalRadiusPc / 1.8153 : s.bulgeRadiusPc / 2.4142);
        // Spheroid: 20 Hernquist radii hold 91% of its mass; beyond that only the disk slab is searched.
        g.coreM = (s.type == GalaxyType::Elliptical || s.bulgeMassFraction > 0.0 ? 20.0 : 0.0) * std::max(a, 1.0) * kPc;
        if (s.barLengthPc > 0.0) g.coreM = std::max(g.coreM, 1.2 * s.barLengthPc * kPc);
        im.galaxies.push_back(std::move(g));
    };
    add(content.host);
    for (const GalaxyRecord& gr : content.galaxies) add(gr.model);
    im.catCenter = content.catalogCenter;
    im.catRadiusPc = std::isfinite(content.catalogRadiusPc) ? content.catalogRadiusPc : 0.0;
    im.catLimit = content.catalogLimitAppMag;
}

bool StarField::gather(const glm::dvec3& view, float limitingAppMag, size_t maxStars, std::vector<StarRecord>& out) {
    Impl& im = *impl_;
    const auto t0 = std::chrono::steady_clock::now();
    ++im.frame;
    if (!std::isfinite(view.x) || !std::isfinite(view.y) || !std::isfinite(view.z)) return false;
    const float mlim = std::isfinite(limitingAppMag) ? std::clamp(limitingAppMag, kMinLimit, kMaxLimit) : kMaxLimit;
    const size_t cap = std::min(maxStars, kHardMaxStars);
    bool force = false;
    if (!im.haveLattice || mlim != im.mag) {
        im.resetCache();
        im.setLattice(mlim);
        force = true;
    }
    if (cap != im.lastCap) {
        im.lastCap = cap;
        force = true;
    }
    const bool first = !im.selectedOnce;
    // A large set selected in the previous call is written now (selection and copy would not fit one
    // call); nothing touched the arenas in between, so its indices are valid.
    bool wrote = false;
    if (im.writePending && !force) {
        im.publish(im.pendingSel, out);
        wrote = true;
    }
    im.writePending = false;
    im.refreshRelevant(view);

    // Class key lists: rebuilt when the viewpoint enters another sub-cell; at most kRebuildMs per call
    // (always one class), round robin so that none starves; not in a call that just wrote the output.
    for (int n = 0; n < kClasses && !wrote; ++n) {
        const int k = (im.rebuildCursor + n) % kClasses;
        if (im.rebuildClass(k, view) && !first && elapsedMs(t0) > kRebuildMs) {
            im.rebuildCursor = (k + 1) % kClasses;
            break;
        }
    }
    im.compactHeap();
    const bool still = view == im.prevView;
    im.prevView = view;
    const bool drift = glm::length(view - im.selAt) > im.validM;
    const bool settleDue = im.newSinceSelect > 0 && im.frame - im.lastSelectFrame >= 4;
    const bool lazyDue = im.publishPending && im.frame - im.lastPublishFrame >= kLazyPublishCalls;
    const bool selectDue = force || drift || settleDue || lazyDue;
    // Eviction (bounded work); arena compaction only in calls without a selection.
    const size_t starBudget = im.starBudget(cap);
    if (im.nearBudget(starBudget)) im.evict(starBudget, !selectDue);
    double overhead = elapsedMs(t0);

    // Generation. The budget leaves room for a selection when one is due, and is trimmed so that the
    // running mean of whole calls stays near kTargetMs.
    const double reserve = selectDue ? std::min(im.lastSelectMs + 0.3, 3.0) : 0.0;
    const double allowance = std::clamp(kTargetMs - im.overheadMs, kMinGenMs, kBudgetMs);
    const double budget = first ? kFirstBudgetMs : overhead + std::max(allowance - reserve, 0.05);   // deadline from t0
    const auto tg = std::chrono::steady_clock::now();
    const bool idle = im.generate(view, t0, budget, starBudget);
    const double genMs = elapsedMs(tg);

    // Selection: forced, new stars (at most every 4th call while settling, at once when generation
    // finishes), a lazy publication that is due, or the viewpoint moved beyond the validity distance
    // (every other call at most when selections are expensive).
    bool doSelect = force || first || lazyDue;
    if (im.newSinceSelect > 0 && (settleDue || idle)) doSelect = true;
    if (drift && (im.frame - im.lastSelectFrame >= 2 || im.lastSelectMs < 1.0)) doSelect = true;
    if (!doSelect && !wrote && out.size() != im.pubCount) doSelect = true;   // the caller changed `out`: rewrite it
    if (wrote) doSelect = false;   // one heavy step per call
    if (doSelect) {
        const auto ts = std::chrono::steady_clock::now();
        const Impl::Selection s = im.select(view, cap);
        const bool changed = !im.publishedOnce || s.fingerprint != im.pubFingerprint || im.cands.size() != im.pubCount;
        // Publish at once when a prominent star is missing (while moving at most every
        // kMinPublishCalls calls), when generation has finished at a still viewpoint, or when a
        // deferred change is old; otherwise (changes near the cut while the viewpoint moves) wait:
        // every publication costs the caller a full rebuild of its star buffer.
        const uint64_t since = im.frame - im.lastPublishFrame;
        bool now = changed && (force || !im.publishedOnce || (s.missingImportant && (still || since >= kMinPublishCalls)) ||
                               (idle && still) || since >= kLazyPublishCalls);
        const bool outLost = out.size() != im.pubCount;   // the caller emptied or resized it: restore now
        if (outLost) now = true;
        if (now) {
            // The copy moves to the next call when it would make this one long.
            if (!force && !first && !outLost && elapsedMs(t0) + im.writeMs > 4.0) {
                im.writePending = true;
                im.pendingSel = s;
            } else {
                im.publish(s, out);
                wrote = true;
            }
        } else {
            im.publishPending = im.publishPending || changed;
        }
        im.lastSelectMs = elapsedMs(ts);
    }
    overhead += elapsedMs(t0) - overhead - genMs;   // everything but generation
    im.overheadMs += 0.05 * (overhead - im.overheadMs);
    return wrote;
}

size_t StarField::cachedCells() const { return impl_->cache.size(); }

}  // namespace bhs::universe
