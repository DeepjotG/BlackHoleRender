// Checks for the galaxy model (src/universe/GalaxyModel.cpp): totals, symmetry, the documented
// spiral-arm convention (every arm count, both signs of the pitch, several phases), ellipticals, the
// dust normalisation (clump factors keep the smooth mean whatever the seed; documented value at R = h),
// robustness against degenerate and non-finite parameters, and the noise statistics the model uses.
// CPU/GPU agreement is checked by the headless EGL harness (see the module's report).
//   g++ -std=c++20 -O2 -Isrc -I<glm> tests/galaxy_model_tests.cpp src/universe/GalaxyModel.cpp src/Units.cpp
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

#include "Units.h"
#include "universe/GalaxyModel.h"
#include "universe/GalaxyModel_internal.h"

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

struct Totals {
    double stars = 0.0, light = 0.0;
};

// Integral over the disk frame on a log-spaced cylindrical grid (R, theta, y).
Totals integrate(const detail::GalaxyEval& g, double lodKpc, int nR = 220, int nT = 48, int nY = 220) {
    Totals t;
    const double rMax = g.boundRadiusPc() / 1000.0, rMin = 1e-4 * rMax, ys = 0.004;
    const double lnR = std::log(rMax / rMin), ay = std::asinh(rMax / ys);
    for (int i = 0; i < nR; ++i) {
        const double R = rMin * std::exp((i + 0.5) / nR * lnR), dR = R * lnR / nR;
        for (int k = 0; k < nY; ++k) {
            const double v = -1.0 + 2.0 * (k + 0.5) / nY;
            const double y = ys * std::sinh(v * ay), dy = ys * std::cosh(v * ay) * ay * 2.0 / nY;
            for (int j = 0; j < nT; ++j) {
                const double th = 2.0 * kPi * (j + 0.5) / nT;
                detail::GalaxySample s;
                g.sampleLocal(R * std::cos(th), y, R * std::sin(th), lodKpc, detail::GalaxyEval::kStars | detail::GalaxyEval::kLight, s);
                const double dV = R * dR * dy * (2.0 * kPi / nT) * 1e9;   // pc^3
                t.stars += s.stars * dV;
                const glm::dvec3 l = s.starLight + s.nebula;
                t.light += (0.2126 * l.x + 0.7152 * l.y + 0.0722 * l.z) * dV;
            }
        }
    }
    return t;
}

GalaxyModel spiral() {
    GalaxyModel m;
    m.type = GalaxyType::Spiral;
    m.seed = 77;
    m.stellarMassSolar = 6e10;
    m.luminositySolar = 2.5e10;
    m.armCount = 2;
    m.armPitchDeg = 12.0;
    m.armRefRadiusPc = 4000.0;
    m.armPhaseDeg = 0.0;
    m.dustExtinctionPerKpc = 3.0;
    return m;
}

// The real universe's Milky Way model (RealUniverse.cpp, milkyWayModel): a long bar, four arms.
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

// Full-detail over smooth midplane dust, area-uniform in the annulus r0 h .. r1 h.
double dustRatio(const GalaxyModel& m, double r0, double r1, int n, uint64_t rngSeed) {
    const detail::GalaxyEval ev(m);
    const double h = m.diskScaleLengthPc / 1000.0;
    std::mt19937_64 rng(rngSeed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double full = 0.0, smooth = 0.0;
    for (int i = 0; i < n; ++i) {
        const double R = std::sqrt(r0 * r0 + (r1 * r1 - r0 * r0) * U(rng)) * h, th = 2.0 * kPi * U(rng);
        detail::GalaxySample a, b;
        ev.sampleLocal(R * std::cos(th), 0.0, R * std::sin(th), 0.0, detail::GalaxyEval::kDust, a);
        ev.sampleLocal(R * std::cos(th), 0.0, R * std::sin(th), 1e3, detail::GalaxyEval::kDust, b);
        full += a.tau;
        smooth += b.tau;
    }
    return full / smooth;
}
}  // namespace

int main() {
    std::printf("Noise statistics the model is normalised with\n");
    {
        std::mt19937_64 rng(5);
        std::uniform_real_distribution<double> U(-300.0, 300.0);
        double m2 = 0.0, hii = 0.0, mg = 0.0;
        const int N = 400000;
        for (int i = 0; i < N; ++i) {
            const double v = detail::valueNoise(static_cast<float>(U(rng)), static_cast<float>(U(rng)), static_cast<float>(U(rng)), 31u);
            m2 += v * v;
            const double t = std::clamp((v - 0.25) / 0.35, 0.0, 1.0), sm = t * t * (3.0 - 2.0 * t);
            hii += sm * sm;
            mg += std::exp(4.0 * v);
        }
        CHECK(std::abs(m2 / N - detail::kNoiseVariance) < 0.004, "noise variance %.4f (model uses %.4f)", m2 / N, detail::kNoiseVariance);
        CHECK(std::abs(hii / N - detail::kHiiMean) < 0.004, "HII knot mask mean %.4f (model uses %.4f)", hii / N, detail::kHiiMean);
        CHECK(std::abs(std::log(mg / N) - detail::lmgf(4.0)) < 0.03, "log E[exp(4 n)] %.3f vs fit %.3f", std::log(mg / N), detail::lmgf(4.0));
        bool wraps = true;
        for (float y = -50.0f; y < 50.0f; y += 0.37f)
            wraps = wraps && std::abs(detail::valueNoiseWrapY(1.3f, y, 2.1f, 9u, 15) - detail::valueNoiseWrapY(1.3f, y + 16.0f, 2.1f, 9u, 15)) < 1e-4f;
        CHECK(wraps, "noise wrapped in y repeats every period (seamless arm-aligned clumps)");
    }

    std::printf("Local normalisation of the dust clumps (the mean holds at every lattice phase)\n");
    {
        // At a fixed fractional position the noise is sum_c w_c v_c over the 8 cell corners; average
        // exp(s n) / norm over many cells at that position. A single global normaliser is off by
        // -30% .. +26% at s = 4 depending on the phase (what a thin layer at one phase sees).
        const float phases[][3] = {{0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.5f}, {0.21f, 0.73f, 0.08f}, {0.5f, 0.0f, 0.37f}};
        std::mt19937_64 rng(17);
        std::uniform_int_distribution<int> I(-200000, 200000);
        double worstExact = 0.0, worstLocal = 0.0, worstGlobal = 0.0;
        for (const auto& f : phases) {
            for (double s : {4.0, 2.4}) {
                double sum = 0.0, sumGlobal = 0.0;
                const int N = 200000;
                glm::vec3 u;
                for (int i = 0; i < N; ++i) {
                    const float x = static_cast<float>(I(rng)) + f[0], y = static_cast<float>(I(rng)) + f[1], z = static_cast<float>(I(rng)) + f[2];
                    const double n = detail::valueNoiseWrapY(x, y, z, 21u, -1, &u);
                    sum += std::exp(s * n) / detail::noiseNormExact(u, s);
                    sumGlobal += std::exp(s * n - detail::lmgf(s));
                }
                worstExact = std::max(worstExact, std::abs(sum / N - 1.0));
                worstGlobal = std::max(worstGlobal, std::abs(sumGlobal / N - 1.0));
            }
            for (double s : {1.6, 1.2, 0.8}) {
                double sum = 0.0;
                const int N = 200000;
                glm::vec3 u;
                for (int i = 0; i < N; ++i) {
                    const float x = static_cast<float>(I(rng)) + f[0], y = static_cast<float>(I(rng)) + f[1], z = static_cast<float>(I(rng)) + f[2];
                    const double n = detail::valueNoiseWrapY(x, y, z, 23u, -1, &u);
                    sum += std::exp(s * n - detail::noiseLmgfLocal(u, s));
                }
                worstLocal = std::max(worstLocal, std::abs(sum / N - 1.0));
            }
        }
        CHECK(worstExact < 0.02, "strong octaves (s = 4, 2.4): exact local mean within %.1f%% at 5 lattice phases (a global normaliser: %.0f%%)",
              100.0 * worstExact, 100.0 * worstGlobal);
        CHECK(worstLocal < 0.005, "weak octaves (s <= 1.6): series normaliser within %.2f%% at every phase", 100.0 * worstLocal);
    }

    std::printf("Dust: documented value and seed independence\n");
    {
        // dustExtinctionPerKpc is the A_V per kpc of the exponential dust disk at R = h; without a bar
        // that is the smooth model's azimuthal mean there, with one the bar's clearing scales it by
        // dustMeanFactor().
        for (int barred = 0; barred < 2; ++barred) {
            const GalaxyModel m = barred ? milkyWay() : spiral();
            const detail::GalaxyEval ev(m);
            const double h = m.diskScaleLengthPc / 1000.0;
            double s = 0.0;
            const int n = 20000;
            for (int i = 0; i < n; ++i) {
                const double th = 2.0 * kPi * (i + 0.5) / n;
                detail::GalaxySample b;
                ev.sampleLocal(h * std::cos(th), 0.0, h * std::sin(th), 1e3, detail::GalaxyEval::kDust, b);
                s += b.tau;
            }
            const double av = 1.086 * s / n * 1000.0, want = m.dustExtinctionPerKpc * ev.dustMeanFactor();
            CHECK(std::abs(av / want - 1.0) < 0.01 && (barred ? ev.dustMeanFactor() < 0.6 : ev.dustMeanFactor() == 1.0),
                  "%s: smooth midplane A_V at R = h is %.3f mag/kpc (parameter %.1f x factor %.3f)", barred ? "barred (Milky Way model)" : "unbarred",
                  av, m.dustExtinctionPerKpc, ev.dustMeanFactor());
        }
        // Full detail against the smooth model for 12 seeds. The clumpy field itself scatters by a few
        // per cent over a disk (a few large clouds hold much of the dust); before the local
        // normalisation the midplane carried a bias of 0.58 .. 1.35 set by the seed.
        double sumDisk = 0.0, worstDisk = 0.0;
        int ringOk = 0;
        const int seeds = 12;
        for (int i = 0; i < seeds; ++i) {
            GalaxyModel m = spiral();
            m.seed = 1000u + 7919u * static_cast<uint32_t>(i);
            const double disk = dustRatio(m, 0.3, 3.0, 150000, 11 + i), ring = dustRatio(m, 0.6, 1.6, 100000, 51 + i);
            sumDisk += disk;
            worstDisk = std::max(worstDisk, std::abs(disk - 1.0));
            ringOk += std::abs(ring - 1.0) < 0.10;
        }
        CHECK(std::abs(sumDisk / seeds - 1.0) < 0.03 && worstDisk < 0.15,
              "12 seeds: midplane dust over the disk (R = 0.3-3 h) is %.3f of the smooth model on average, every seed within %.1f%%", sumDisk / seeds,
              100.0 * worstDisk);
        CHECK(ringOk >= 10, "%d of 12 seeds within 10%% of the documented value on the ring R = 0.6-1.6 h", ringOk);
        double sumMw = 0.0;
        for (int i = 0; i < 12; ++i) {
            GalaxyModel m = milkyWay();
            m.seed += 31u * static_cast<uint32_t>(i);
            sumMw += dustRatio(m, 0.3, 3.0, 150000, 91 + i);
        }
        CHECK(std::abs(sumMw / 12.0 - 1.0) < 0.05, "barred: 12 seeds of the Milky Way model, midplane dust %.3f of the smooth model on average",
              sumMw / 12.0);
    }

    std::printf("Totals (smooth and with full detail)\n");
    {
        const GalaxyModel types[] = {
            spiral(),
            [] {
                GalaxyModel m = spiral();
                m.type = GalaxyType::BarredSpiral;
                m.barLengthPc = 4000.0;
                m.barAngleDeg = 30.0;
                m.armRefRadiusPc = 4000.0;
                m.armPhaseDeg = 30.0;
                return m;
            }(),
            [] {
                GalaxyModel m;
                m.type = GalaxyType::Elliptical;
                m.stellarMassSolar = 2e11;
                m.luminositySolar = 5e10;
                m.ellipticalRadiusPc = 6000.0;
                m.ellipticalAxisRatio = 0.7;
                return m;
            }(),
            [] {
                GalaxyModel m;
                m.type = GalaxyType::Irregular;
                m.stellarMassSolar = 2e9;
                m.luminositySolar = 1.5e9;
                m.diskScaleLengthPc = 1500.0;
                m.diskScaleHeightPc = 450.0;
                m.diskCutoffPc = 7000.0;
                m.bulgeMassFraction = 0.0;
                m.armContrast = 1.3;
                return m;
            }(),
            milkyWay(),
        };
        const char* names[] = {"spiral", "barred spiral", "elliptical", "irregular", "Milky Way model"};
        for (int i = 0; i < 5; ++i) {
            const detail::GalaxyEval g(types[i]);
            const Totals smooth = integrate(g, 1e3);
            const double n = types[i].stellarMassSolar / 0.4, l = types[i].luminositySolar;
            CHECK(std::abs(smooth.stars / n - 1.0) < 0.12, "%s: stars integrate to %.3f of M/0.4", names[i], smooth.stars / n);
            CHECK(std::abs(smooth.light / l - 1.0) < 0.2, "%s: light integrates to %.3f of L", names[i], smooth.light / l);
            if (i == 0 || i == 3 || i == 4) {
                const Totals full = integrate(g, 0.0, 160, 64, 160);
                CHECK(std::abs(full.light / smooth.light - 1.0) < 0.15, "%s: full detail keeps the light total (%.3f of smooth)", names[i],
                      full.light / smooth.light);
            }
        }
    }

    std::printf("Spiral arms\n");
    {
        const GalaxyModel m = spiral();
        const detail::GalaxyEval g(m);
        bool symmetric = true;
        for (double R : {2.0, 5.0, 9.0})
            for (double th = 0.0; th < 3.2; th += 0.4) {
                detail::GalaxySample a, b;
                g.sampleLocal(R * std::cos(th), 0.05, R * std::sin(th), 1e3, detail::GalaxyEval::kAll, a);
                g.sampleLocal(R * std::cos(th + kPi), 0.05, R * std::sin(th + kPi), 1e3, detail::GalaxyEval::kAll, b);
                symmetric = symmetric && std::abs(a.stars - b.stars) <= 1e-9 * a.stars && std::abs(a.tau - b.tau) <= 1e-9 * a.tau + 1e-15;
            }
        CHECK(symmetric, "a two-armed spiral is symmetric under a half turn (smooth model)");

        // On the documented arm curve psi = 0 versus half an arm spacing away (psi = pi), full detail,
        // for 1-6 arms, both winding senses and two phases.
        int combos = 0, perfect = 0, worst = 1 << 30, worstTotal = 0;
        for (int arms : {1, 2, 3, 4, 6})
            for (double pitch : {-20.0, -8.0, 5.0, 12.0, 25.0, 45.0})
                for (double phase : {0.0, 77.0}) {
                    GalaxyModel a;
                    a.armCount = arms;
                    a.armPitchDeg = pitch;
                    a.armPhaseDeg = phase;
                    a.seed = static_cast<uint32_t>(arms * 1000 + static_cast<int>(pitch + 100) * 7 + static_cast<int>(phase));
                    const glm::dvec3 major = a.major, minor = glm::cross(a.normal, a.major);
                    int better = 0, total = 0;
                    for (double R = 2.5; R <= 13.0; R += 0.25) {
                        const double th = std::log(R * 1000.0 / a.armRefRadiusPc) / std::tan(pitch * kPi / 180.0) + phase * kPi / 180.0;
                        const auto at = [&](double t) { return (R * 1000.0 * units::kParsec) * (std::cos(t) * major + std::sin(t) * minor); };
                        ++total;
                        if (a.youngFraction(at(th)) > a.youngFraction(at(th + kPi / arms))) ++better;
                    }
                    ++combos;
                    perfect += better == total;
                    if (better < worst) {
                        worst = better;
                        worstTotal = total;
                    }
                }
        CHECK(perfect == combos, "young stars peak on the documented arm curve: %d of %d combinations at every radius (worst %d of %d)", perfect, combos,
              worst, worstTotal);
        CHECK(std::log(8.0 / 4.0) / std::tan(12.0 * kPi / 180.0) > 0.0, "positive pitch: the arm's azimuth increases outward");
    }

    std::printf("Ellipticals, robustness, packing\n");
    {
        GalaxyModel e;
        e.type = GalaxyType::Elliptical;
        e.ellipticalRadiusPc = 3000.0;
        const glm::dvec3 p(1000.0 * units::kParsec, 200.0 * units::kParsec, -500.0 * units::kParsec);
        CHECK(e.youngFraction(p) == 0.0 && e.dustExtinction(p) == 0.0, "ellipticals have no young stars and no dust");
        const auto allFinite = [](const GalaxyModel& m, std::mt19937_64& rng) {
            std::uniform_real_distribution<double> U(-1.0, 1.0);
            bool ok = true;
            for (int i = 0; i < 1500; ++i) {
                const double scale = std::pow(10.0, 3.0 * U(rng) + 1.0) * units::kParsec;   // 10 pc .. 10 kpc and beyond
                const glm::dvec3 q = i == 0 ? glm::dvec3(0.0) : glm::dvec3(U(rng), U(rng), U(rng)) * scale;
                const double d = m.stellarDensity(q), x = m.dustExtinction(q), y = m.youngFraction(q);
                const glm::vec3 l = m.emissivity(q);
                ok = ok && std::isfinite(d) && d >= 0.0 && std::isfinite(x) && x >= 0.0 && y >= 0.0 && y <= 1.0 && std::isfinite(l.x) &&
                     std::isfinite(l.y) && std::isfinite(l.z) && l.x >= 0.0f && l.y >= 0.0f && l.z >= 0.0f;
            }
            const GalaxyGpu gp = packGalaxyGpu(m, glm::dvec3(0.0));
            for (const glm::vec4& v : gp.data) ok = ok && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
            return ok && std::isfinite(m.boundingRadiusPc()) && m.boundingRadiusPc() > 0.0;
        };
        std::mt19937_64 rng(3);
        bool finite = true;
        for (int type = 0; type < 5; ++type) {
            GalaxyModel m = spiral();
            m.type = static_cast<GalaxyType>(type);
            if (type == 1) m.barLengthPc = 3000.0;
            finite = finite && allFinite(m, rng);
        }
        CHECK(finite, "finite, non-negative values everywhere for every type, including the centre");
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        struct Case {
            const char* name;
            void (*apply)(GalaxyModel&, double, double);
        };
        const Case cases[] = {
            {"lengths 0", [](GalaxyModel& m, double, double) { m.diskScaleLengthPc = m.diskScaleHeightPc = m.diskCutoffPc = m.bulgeRadiusPc = m.dustScaleHeightPc = 0; }},
            {"negative lengths", [](GalaxyModel& m, double, double) { m.diskScaleLengthPc = m.diskScaleHeightPc = m.bulgeRadiusPc = -100; m.diskCutoffPc = -5; }},
            {"arm count 0 / -3 / 1000", [](GalaxyModel& m, double, double) { m.armCount = (m.seed % 3 == 0) ? 0 : (m.seed % 3 == 1 ? -3 : 1000); }},
            {"pitch 0 / 90 / -90", [](GalaxyModel& m, double, double) { m.armPitchDeg = (m.seed % 3 == 0) ? 0.0 : (m.seed % 3 == 1 ? 90.0 : -90.0); }},
            {"zero and parallel axes", [](GalaxyModel& m, double, double) { m.normal = glm::dvec3(0.0); m.major = m.seed % 2 ? glm::dvec3(0.0) : m.normal; }},
            {"mass 0 / 1e15", [](GalaxyModel& m, double, double) { m.stellarMassSolar = m.luminositySolar = m.seed % 2 ? 0.0 : 1e15; }},
            {"invalid type 7", [](GalaxyModel& m, double, double) { m.type = static_cast<GalaxyType>(7); }},
            {"NaN pitch, length, mass, width", [](GalaxyModel& m, double n, double) { m.armPitchDeg = m.diskScaleLengthPc = m.stellarMassSolar = m.armWidth = n; }},
            {"NaN/inf everywhere", [](GalaxyModel& m, double n, double i) {
                 m.center = glm::dvec3(n);
                 m.normal = glm::dvec3(i, 0.0, 0.0);
                 m.luminositySolar = i;
                 m.diskScaleHeightPc = m.diskCutoffPc = m.bulgeMassFraction = m.bulgeRadiusPc = m.bulgeFlattening = n;
                 m.barLengthPc = m.barAxisRatio = m.barAngleDeg = m.armRefRadiusPc = m.armPhaseDeg = m.armContrast = n;
                 m.dustScaleHeightPc = m.dustExtinctionPerKpc = m.ellipticalRadiusPc = m.ellipticalAxisRatio = i;
                 m.diskColor = glm::vec3(static_cast<float>(n));
             }},
        };
        int badCases = 0;
        for (const Case& c : cases)
            for (int type = 0; type < 5; ++type)
                for (uint32_t variant = 0; variant < 3; ++variant) {
                    GalaxyModel m;
                    m.type = static_cast<GalaxyType>(type);
                    m.seed = variant;
                    m.dustExtinctionPerKpc = 3.0;
                    c.apply(m, nan, inf);
                    if (!allFinite(m, rng)) {
                        ++badCases;
                        std::printf("        not finite: %s, type %d, variant %u\n", c.name, type, variant);
                    }
                }
        CHECK(badCases == 0, "degenerate and non-finite parameters give finite values, packing and bounding radius (%d cases)", 9 * 5 * 3);
        const GalaxyGpu gpu = packGalaxyGpu(spiral(), glm::dvec3(8000.0 * units::kParsec, 0.0, 0.0));
        CHECK(std::abs(gpu.data[0].x + 8.0f) < 1e-4f && std::abs(gpu.data[1].w - static_cast<float>(spiral().boundingRadiusPc() / 1000.0)) < 1e-3f,
              "GPU packing: centre relative to the viewpoint in kpc, bounding radius");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
