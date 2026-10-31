// Checks for the procedural star field (src/universe/StarField.cpp): determinism, magnitudes and the
// cap (brightest first, coverage at the app's settings: limit 9, 200k stars), counts per class versus
// the density model, catalogue suppression, change detection, and memory and time per call (settling
// at galaxy centres, a slow drift and a fast flight). Times are thread CPU times, so the checks hold
// on a busy machine (the field's own budget is wall-clock time, which only gives less work per call).
//   g++ -std=c++20 -O2 -Isrc -I<glm> tests/starfield_tests.cpp src/universe/StarField.cpp
//       src/universe/InventedUniverse.cpp src/universe/GalaxyModel.cpp src/Units.cpp -o starfield_tests
#include <malloc.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <tuple>
#include <vector>

#include "Units.h"
#include "universe/GalaxyModel_internal.h"
#include "universe/StarField.h"
#include "universe/StarField_internal.h"
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

constexpr double kPc = units::kParsec;
constexpr float kAppLimit = 9.0f;
constexpr size_t kAppCap = 200000;

double cpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

// Heap in use (glibc): unlike the resident set it drops when a field is destroyed.
double heapMB() {
    const struct mallinfo2 mi = mallinfo2();
    return static_cast<double>(mi.uordblks + mi.hblkhd) / 1048576.0;
}

double appMag(const StarRecord& s, const glm::dvec3& at) {
    return s.absMag + 5.0 * std::log10(std::max(glm::length(s.pos - at) / kPc, 1e-3) / 10.0);
}

struct Run {
    std::vector<StarRecord> out;
    int calls = 0;
    double firstMs = 0.0, maxMs = 0.0, sumMs = 0.0;   // CPU per call (max excludes the first)
    bool settled = false;
};

// Gathers at `at` until the cache has not changed and nothing was published for 40 calls.
Run settle(StarField& f, const glm::dvec3& at, float mlim, size_t cap, int maxCalls = 8000) {
    Run r;
    size_t lastCells = static_cast<size_t>(-1);
    int quiet = 0;
    for (int i = 0; i < maxCalls; ++i) {
        const double t = cpuMs();
        const bool changed = f.gather(at, mlim, cap, r.out);
        const double ms = cpuMs() - t;
        if (i == 0) r.firstMs = ms;
        else r.maxMs = std::max(r.maxMs, ms);
        r.sumMs += ms;
        ++r.calls;
        quiet = (!changed && f.cachedCells() == lastCells) ? quiet + 1 : 0;
        lastCells = f.cachedCells();
        if (quiet >= 40) {
            r.settled = true;
            break;
        }
    }
    return r;
}

using Id = std::tuple<double, double, double, float>;
std::vector<Id> ids(const std::vector<StarRecord>& v) {
    std::vector<Id> r;
    r.reserve(v.size());
    for (const StarRecord& s : v) r.emplace_back(s.pos.x, s.pos.y, s.pos.z, s.absMag);
    std::sort(r.begin(), r.end());
    return r;
}
bool sameStars(const std::vector<StarRecord>& a, const std::vector<StarRecord>& b) { return ids(a) == ids(b); }

// The real universe's Milky Way model (RealUniverse.cpp, milkyWayModel), for its centre.
GalaxyModel milkyWay() {
    GalaxyModel m;
    m.type = GalaxyType::BarredSpiral;
    m.seed = 0x4D696C6Bu;
    m.stellarMassSolar = 5.0e10;
    m.luminositySolar = 3.0e10;
    m.diskScaleLengthPc = 2600.0;
    m.diskScaleHeightPc = 300.0;
    m.diskCutoffPc = 15000.0;
    m.bulgeMassFraction = 0.25;
    m.bulgeRadiusPc = 800.0;
    m.bulgeFlattening = 0.6;
    m.barLengthPc = 4800.0;
    m.barAxisRatio = 0.35;
    m.barAngleDeg = 153.0;
    m.armCount = 4;
    m.armPitchDeg = 12.0;
    m.armRefRadiusPc = 7050.0;
    m.armWidth = 0.4;
    m.armContrast = 3.0;
    m.dustScaleHeightPc = 110.0;
    m.dustExtinctionPerKpc = 5.0;
    return m;
}

// Brightest first: `small` is the `small.size()` brightest of `big` (seen from `at`).
void checkBrightestFirst(const char* where, const std::vector<StarRecord>& small, const std::vector<StarRecord>& big, const glm::dvec3& at) {
    const std::vector<Id> s = ids(small), b = ids(big);
    std::vector<Id> missing, dropped;
    std::set_difference(s.begin(), s.end(), b.begin(), b.end(), std::back_inserter(missing));
    std::set_difference(b.begin(), b.end(), s.begin(), s.end(), std::back_inserter(dropped));
    double faintestKept = -100.0, brightestDropped = 100.0;
    for (const StarRecord& st : small) faintestKept = std::max(faintestKept, appMag(st, at));
    for (const Id& id : dropped) {
        StarRecord st;
        st.pos = glm::dvec3(std::get<0>(id), std::get<1>(id), std::get<2>(id));
        st.absMag = std::get<3>(id);
        brightestDropped = std::min(brightestDropped, appMag(st, at));
    }
    CHECK(missing.empty() && faintestKept <= brightestDropped + 1e-4,
          "%s: every published star (faintest V = %.3f) is brighter than every dropped one (brightest V = %.3f); %zu published not in the larger set",
          where, faintestKept, brightestDropped, missing.size());
}

}  // namespace

int main() {
    UniverseContent u;
    buildInventedUniverse(u, 2024u);
    const glm::dvec3 home = u.blackHolePos;

    std::printf("Determinism (limit 7)\n");
    std::vector<StarRecord> sa;
    {
        StarField a, b, c, d;
        a.configure(u, 9u);
        b.configure(u, 9u);
        c.configure(u, 10u);
        d.configure(u, 9u);
        sa = settle(a, home, 7.0f, 400000).out;
        CHECK(!sa.empty() && sameStars(sa, settle(b, home, 7.0f, 400000).out), "two fields with the same seed give the same %zu stars", sa.size());
        CHECK(!sameStars(sa, settle(c, home, 7.0f, 400000).out), "another seed gives other stars");
        settle(d, home + glm::dvec3(30.0 * kPc, 0.0, 0.0), 7.0f, 400000);
        CHECK(sameStars(sa, settle(d, home, 7.0f, 400000).out), "stars do not depend on where the field was seen from before");
    }

    std::printf("Magnitudes and physical values\n");
    {
        double worst = -100.0;
        bool sane = true;
        size_t hot = 0, cool = 0;
        for (const StarRecord& s : sa) {
            worst = std::max(worst, appMag(s, home));
            sane = sane && std::isfinite(s.teff) && s.teff > 2000.0f && s.teff < 50000.0f && s.radiusSolar > 0.0f && s.radiusSolar < 3000.0f &&
                   s.absMag > -9.0f && s.absMag < 17.0f && s.name == -1;
            (s.teff > 10000.0f ? hot : cool) += 1;
        }
        CHECK(worst < 7.0 + detail::kStarPubMargin + 1e-3, "uncapped, the published stars reach the limit plus the %.1f mag margin (faintest %.3f, limit 7)",
              detail::kStarPubMargin, worst);
        CHECK(sane, "temperatures, radii and magnitudes are physical");
        CHECK(hot > 0 && cool > 0, "both hot young stars (%zu) and cool giants/dwarfs (%zu) appear", hot, cool);
    }

    std::printf("Cap at the app's settings (limit 9, 200k): brightest first and coverage\n");
    std::vector<StarRecord> homeCapped;
    for (uint32_t seed : {2024u, 123456u}) {
        UniverseContent v;
        buildInventedUniverse(v, seed);
        const glm::dvec3 at = v.blackHolePos;
        StarField capped, full;
        capped.configure(v, seed);
        full.configure(v, seed);
        const Run rc = settle(capped, at, kAppLimit, kAppCap);
        const Run rf = settle(full, at, kAppLimit, static_cast<size_t>(1e9));
        size_t visible = 0, covered = 0;
        const std::vector<Id> pub = ids(rc.out);
        for (const StarRecord& s : rf.out)
            if (appMag(s, at) < kAppLimit) {
                ++visible;
                covered += std::binary_search(pub.begin(), pub.end(), Id(s.pos.x, s.pos.y, s.pos.z, s.absMag));
            }
        std::printf("  seed %u: %zu published (cap), %zu uncapped (%zu brighter than 9), settled in %d calls\n", seed, rc.out.size(), rf.out.size(),
                    visible, rc.calls);
        CHECK(rc.settled && rc.out.size() == std::min(kAppCap, rf.out.size()), "seed %u: the cap is filled (%zu stars)", seed, rc.out.size());
        char where[64];
        std::snprintf(where, sizeof where, "seed %u, black hole", seed);
        checkBrightestFirst(where, rc.out, rf.out, at);
        // All of them when they fit under the cap, otherwise the cap is full of them.
        const size_t room = std::min(visible, kAppCap);
        CHECK(covered >= 0.99 * static_cast<double>(room), "seed %u: %zu of the uncapped field's %zu stars brighter than 9 are published (%.2f%% of %zu)",
              seed, covered, visible, 100.0 * covered / std::max<size_t>(room, 1), room);
        // The published stars of each class lie on both sides of the viewpoint (no cut by a plane).
        int lopsided = 0;
        for (int k = 0; k < detail::kStarClasses; ++k) {
            size_t px = 0, nx = 0, fx = 0, fnx = 0;
            for (const StarRecord& s : rc.out)
                if (static_cast<int>(std::floor(s.absMag)) - detail::kStarMinMag == k) ((s.pos - at).x > 0.0 ? px : nx) += 1;
            for (const StarRecord& s : rf.out)
                if (static_cast<int>(std::floor(s.absMag)) - detail::kStarMinMag == k && appMag(s, at) < kAppLimit) ((s.pos - at).x > 0.0 ? fx : fnx) += 1;
            if (px + nx > 2000 && fx + fnx > 2000) {
                const double r1 = static_cast<double>(px) / std::max<size_t>(nx, 1), r2 = static_cast<double>(fx) / std::max<size_t>(fnx, 1);
                if (std::abs(std::log(r1 / r2)) > 0.1) ++lopsided;
            }
        }
        CHECK(lopsided == 0, "seed %u: per class, the +x / -x balance of the published stars matches the uncapped field", seed);
        if (seed == 2024u) {
            homeCapped = rc.out;
            std::printf("  cost while settling: first call %.2f ms, then at most %.2f ms, mean %.2f ms (CPU)\n", rc.firstMs, rc.maxMs, rc.sumMs / rc.calls);
            // Change detection: a settled field reports no change; a cleared output is restored.
            bool quiet = true;
            for (int i = 0; i < 60; ++i) quiet = !capped.gather(at, kAppLimit, kAppCap, homeCapped) && quiet;
            CHECK(quiet && sameStars(homeCapped, rc.out), "a second gather at the same settled viewpoint returns false (60 calls) and keeps the set");
            std::vector<StarRecord> cleared;
            const bool restored = capped.gather(at, kAppLimit, kAppCap, cleared);
            CHECK(restored && sameStars(cleared, rc.out), "an emptied output vector is refilled with the same set (and reported)");
        }
    }

    std::printf("Counts per class versus the density model (uncapped, limit 9, from the black hole)\n");
    {
        StarField f;
        f.configure(u, 2024u);
        const std::vector<StarRecord> all = settle(f, home, kAppLimit, static_cast<size_t>(1e9)).out;
        const detail::GalaxyEval host(u.host);
        std::mt19937_64 rng(99);
        std::uniform_real_distribution<double> U(-1.0, 1.0);
        int tested = 0, bad = 0;
        double worstSigma = 0.0;
        for (int k = 0; k < detail::kStarClasses; ++k) {
            // Every class-k star within the faintest member's reach of limit + margin is published.
            const double rPc = std::pow(10.0, (kAppLimit + detail::kStarPubMargin - (detail::kStarMinMag + k + 1) + 5.0) / 5.0);
            const double r = std::min(rPc, 1500.0);
            if (r < 2.0) continue;
            size_t gen = 0;
            for (const StarRecord& s : all)
                if (s.absMag >= detail::kStarMinMag + k && s.absMag < detail::kStarMinMag + k + 1 && glm::length(s.pos - home) < r * kPc) ++gen;
            const int N = 400000;
            double sum = 0.0;
            for (int i = 0; i < N; ++i) {
                glm::dvec3 d;
                do d = glm::dvec3(U(rng), U(rng), U(rng));
                while (glm::dot(d, d) > 1.0);
                detail::GalaxySample s;
                host.sample(home + d * (r * kPc), s, 0.0, detail::GalaxyEval::kStars);
                sum += std::max(s.stars - s.youngStars, 0.0) * detail::starClassFraction(k, false) + s.youngStars * detail::starClassFraction(k, true);
            }
            const double expected = sum / N * (4.0 / 3.0 * 3.14159265358979 * r * r * r);
            if (expected < 20.0) continue;
            ++tested;
            // Poisson noise of the stars plus the Monte Carlo's own noise (a few per cent in a thin disk).
            const double sigma = std::sqrt(expected + std::pow(0.03 * expected, 2.0));
            const double z = (static_cast<double>(gen) - expected) / sigma;
            worstSigma = std::max(worstSigma, std::abs(z));
            if (std::abs(z) > 4.0) ++bad;
            std::printf("    M_V %+3d: %7zu stars within %6.0f pc, density model %9.1f (%+.1f sigma)\n", detail::kStarMinMag + k, gen, r, expected, z);
        }
        CHECK(tested >= 12 && bad == 0, "%d classes match the density integral within 4 sigma (worst %.1f)", tested, worstSigma);
        size_t naked = 0;
        for (const StarRecord& s : all) naked += appMag(s, home) < 6.5;
        CHECK(naked > 1500 && naked < 60000, "%zu stars brighter than 6.5 before extinction (sky: ~9000 after it)", naked);
    }

    std::printf("Galaxy centres: brightest first under the cap, memory, time per call\n");
    {
        struct Place {
            const char* name;
            UniverseContent content;
            glm::dvec3 at;
        };
        std::vector<Place> places;
        places.push_back({"invented host centre", u, u.host.center + glm::dvec3(1.0, 2.0, 3.0) * kPc});
        UniverseContent mw;
        mw.mode = Mode::Real;
        mw.host = milkyWay();
        places.push_back({"Milky Way model centre", mw, glm::dvec3(0.0)});
        for (const Place& p : places) {
            const double heap0 = heapMB();
            Run r200, r300;
            {
                StarField f;
                f.configure(p.content, 7u);
                r200 = settle(f, p.at, kAppLimit, kAppCap);
                const double heap = heapMB() - heap0;
                CHECK(r200.settled && r200.out.size() == kAppCap, "%s: settles with %zu stars in %d calls", p.name, r200.out.size(), r200.calls);
                CHECK(heap > 1.0 && heap < 300.0, "%s: the field and its output use %.0f MB", p.name, heap);
                CHECK(r200.maxMs < 10.0 && r200.sumMs / r200.calls < 3.0, "%s: first call %.2f ms, then every call < 10 ms (max %.2f, mean %.2f)", p.name,
                      r200.firstMs, r200.maxMs, r200.sumMs / r200.calls);
            }
            {
                StarField f;
                f.configure(p.content, 7u);
                r300 = settle(f, p.at, kAppLimit, 300000);
            }
            checkBrightestFirst(p.name, r200.out, r300.out, p.at);
        }
    }

    std::printf("Travel (limit 9, 200k): slow drift and a fast flight through the host's centre\n");
    {
        StarField f;
        f.configure(u, 2024u);
        std::vector<StarRecord> out = settle(f, home, kAppLimit, kAppCap).out, prev = out;
        const glm::dvec3 dir = glm::normalize(u.host.center - home);
        for (double step : {0.5, 20.0}) {
            const int calls = step < 1.0 ? 800 : 400;
            const glm::dvec3 start = home + dir * (step < 1.0 ? 0.0 : 1.0) * kPc;
            double maxMs = 0.0, sumMs = 0.0;
            int trues = 0, sameAsBefore = 0;
            size_t minOut = out.size();
            for (int i = 0; i < calls; ++i) {
                const glm::dvec3 at = start + dir * (step * i * kPc);
                const double t = cpuMs();
                const bool changed = f.gather(at, kAppLimit, kAppCap, out);
                const double ms = cpuMs() - t;
                maxMs = std::max(maxMs, ms);
                sumMs += ms;
                if (changed) {
                    ++trues;
                    sameAsBefore += out.size() == prev.size() && std::equal(out.begin(), out.end(), prev.begin(), [](const StarRecord& a, const StarRecord& b) {
                                        return a.pos == b.pos && a.absMag == b.absMag;
                                    });
                    prev = out;
                }
                minOut = std::min(minOut, out.size());
            }
            CHECK(maxMs < 10.0 && sumMs / calls < 3.0, "%.1f pc per call: every call < 10 ms (max %.2f), mean %.2f ms (CPU)", step, maxMs, sumMs / calls);
            CHECK(sameAsBefore == 0, "%.1f pc per call: %d changes reported, none of them with the previous set", step, trues);
            if (step < 1.0) CHECK(minOut >= 0.95 * kAppCap, "0.5 pc per call: generation keeps up (at least %zu stars published)", minOut);
        }
    }

    std::printf("Far from galaxies, catalogue suppression\n");
    {
        StarField g;
        g.configure(u, 9u);
        const glm::dvec3 voidPoint = home + glm::normalize(glm::dvec3(0.3, 0.9, 0.2)) * (400.0 * 1e3 * kPc);
        const Run r = settle(g, voidPoint, kAppLimit, kAppCap);
        CHECK(r.out.empty() && g.cachedCells() < 100, "intergalactic space is empty (%zu stars, %zu boxes)", r.out.size(), g.cachedCells());

        // The catalogue holds the stars observed brighter than its limit (dimmed by the host's dust,
        // Universe.h): procedural stars there are suppressed, those that dust hides below it are kept.
        UniverseContent v = u;
        v.catalogCenter = home;
        v.catalogRadiusPc = 300.0;
        v.catalogLimitAppMag = 6.0f;
        StarField f;
        f.configure(v, 9u);
        int duplicated = 0, hidden = 0;
        for (const StarRecord& s : settle(f, home, 7.0f, 400000).out) {
            const double dPc = glm::length(s.pos - home) / kPc;
            const double m = appMag(s, home);
            if (dPc >= 300.0 || m >= 6.0) continue;
            const int n = std::max(1, static_cast<int>(dPc / 2.0));
            double av = 0.0;
            for (int k = 0; k < n; ++k) av += v.host.dustExtinction(home + (s.pos - home) * ((k + 0.5) / n)) * (dPc / n);
            if (m + av < 6.0 - 0.3) ++duplicated;
            else ++hidden;
        }
        CHECK(duplicated == 0 && hidden > 0,
              "no procedural star duplicates the catalogue region (r < 300 pc, observed V < 6); %d kept that dust dims below the limit", hidden);
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
