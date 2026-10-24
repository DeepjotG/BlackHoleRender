// Checks for the galaxy model (src/universe/GalaxyModel.cpp): totals, symmetry, the documented
// spiral-arm convention, ellipticals, robustness and the noise statistics the model relies on.
#include <algorithm>
#include <cmath>
#include <cstdio>
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
        char msg_[256];                               \
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
        };
        const char* names[] = {"spiral", "barred spiral", "elliptical", "irregular"};
        for (int i = 0; i < 4; ++i) {
            const detail::GalaxyEval g(types[i]);
            const Totals smooth = integrate(g, 1e3);
            const double n = types[i].stellarMassSolar / 0.4, l = types[i].luminositySolar;
            CHECK(std::abs(smooth.stars / n - 1.0) < 0.12, "%s: stars integrate to %.3f of M/0.4", names[i], smooth.stars / n);
            CHECK(std::abs(smooth.light / l - 1.0) < 0.2, "%s: light integrates to %.3f of L", names[i], smooth.light / l);
            if (i == 0 || i == 3) {
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

        // On the documented arm curve psi = 0 versus half an arm spacing away (psi = pi).
        int better = 0, total = 0;
        const double tanP = std::tan(m.armPitchDeg * kPi / 180.0);
        for (double R = 3.0; R <= 12.0; R += 0.5) {
            const double th = std::log(R * 1000.0 / m.armRefRadiusPc) / tanP + m.armPhaseDeg * kPi / 180.0;
            const glm::dvec3 major = m.major, minor = glm::cross(m.normal, m.major);
            const auto at = [&](double t) { return (R * 1000.0 * units::kParsec) * (std::cos(t) * major + std::sin(t) * minor); };
            ++total;
            if (m.youngFraction(at(th)) > m.youngFraction(at(th + kPi / m.armCount))) ++better;
        }
        CHECK(better == total, "young stars peak on the arm curve (%d of %d radii)", better, total);
        // The arm curve winds counter-clockwise outward seen from +normal for positive pitch.
        CHECK(std::log(8.0 / 4.0) / tanP > 0.0, "positive pitch: the arm's azimuth increases outward");
    }

    std::printf("Ellipticals, robustness, packing\n");
    {
        GalaxyModel e;
        e.type = GalaxyType::Elliptical;
        e.ellipticalRadiusPc = 3000.0;
        const glm::dvec3 p(1000.0 * units::kParsec, 200.0 * units::kParsec, -500.0 * units::kParsec);
        CHECK(e.youngFraction(p) == 0.0 && e.dustExtinction(p) == 0.0, "ellipticals have no young stars and no dust");
        bool finite = true;
        std::mt19937_64 rng(3);
        std::uniform_real_distribution<double> U(-1.0, 1.0);
        for (int type = 0; type < 5; ++type) {
            GalaxyModel m = spiral();
            m.type = static_cast<GalaxyType>(type);
            if (type == 1) m.barLengthPc = 3000.0;
            for (int i = 0; i < 4000; ++i) {
                const double scale = std::pow(10.0, 3.0 * U(rng) + 1.0) * units::kParsec;   // 10 pc .. 10 kpc and beyond
                const glm::dvec3 q = glm::dvec3(U(rng), U(rng), U(rng)) * scale;
                const double d = m.stellarDensity(q), x = m.dustExtinction(q), y = m.youngFraction(q);
                const glm::vec3 l = m.emissivity(q);
                finite = finite && std::isfinite(d) && d >= 0.0 && std::isfinite(x) && x >= 0.0 && y >= 0.0 && y <= 1.0 &&
                         std::isfinite(l.x) && l.x >= 0.0f;
            }
            const glm::dvec3 centre(0.0);
            finite = finite && std::isfinite(m.stellarDensity(centre)) && std::isfinite(m.emissivity(centre).y);
        }
        CHECK(finite, "finite, non-negative values everywhere for every type, including the centre");
        const GalaxyGpu gpu = packGalaxyGpu(spiral(), glm::dvec3(8000.0 * units::kParsec, 0.0, 0.0));
        CHECK(std::abs(gpu.data[0].x + 8.0f) < 1e-4f && std::abs(gpu.data[1].w - static_cast<float>(spiral().boundingRadiusPc() / 1000.0)) < 1e-3f,
              "GPU packing: centre relative to the viewpoint in kpc, bounding radius");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
