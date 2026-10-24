#include "UniverseScene.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <random>

#include "Log.h"
#include "Orbits.h"
#include "Settings.h"
#include "Units.h"
#if !BHS_GALAXY_MODEL_STUB
#include "universe/GalaxyModel_internal.h"
#endif

namespace bhs {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Surface brightness (L_sun / pc^2 / sr) -> HDR radiance. Calibrated so the Milky Way band seen
// from the Sun has about the brightness of the painted backdrop's band.
constexpr double kSurfaceToHdr = 0.006;
constexpr float kLimitingMag = 9.0f;
constexpr size_t kMaxProcStars = 200000;
constexpr int kSpriteGrid = 16;
constexpr int kMaxNearGalaxies = 16;

struct StarGpuIn {
    glm::vec4 posMag;
    glm::vec4 colorRadius;
};
struct StarGpuOut {
    glm::vec4 dirFlux;
    glm::vec4 color;
};
struct SpriteGpu {
    glm::vec4 dirCos, axisQ, color, params;
};

// Linear-sRGB colour of a blackbody, normalised to luminance 1 (same CIE fit as DiskTextures).
double lobe(double l, double mu, double s1, double s2) {
    const double t = (l - mu) / (l < mu ? s1 : s2);
    return std::exp(-0.5 * t * t);
}
glm::vec3 blackbodyColor(double T) {
    static std::vector<glm::vec3> table;
    if (table.empty()) {
        for (int i = 0; i < 128; ++i) {
            const double t = std::exp(std::log(1000.0) + (std::log(50000.0) - std::log(1000.0)) * i / 127.0);
            double X = 0, Y = 0, Z = 0;
            for (int l = 380; l <= 780; l += 5) {
                const double x = 1.056 * lobe(l, 599.8, 37.9, 31.0) + 0.362 * lobe(l, 442.0, 16.0, 26.7) - 0.065 * lobe(l, 501.1, 20.4, 26.2);
                const double y = 0.821 * lobe(l, 568.8, 46.9, 40.5) + 0.286 * lobe(l, 530.9, 16.3, 31.1);
                const double z = 1.217 * lobe(l, 437.0, 11.8, 36.0) + 0.681 * lobe(l, 459.0, 26.0, 13.8);
                const double lm = l * 1e-9;
                const double b = 1.0 / (lm * lm * lm * lm * lm * (std::exp(1.438777e-2 / (lm * t)) - 1.0));
                X += b * x; Y += b * y; Z += b * z;
            }
            double r = std::max(0.0, 3.2406 * X - 1.5372 * Y - 0.4986 * Z);
            double g = std::max(0.0, -0.9689 * X + 1.8758 * Y + 0.0415 * Z);
            double bl = std::max(0.0, 0.0557 * X - 0.2040 * Y + 1.0570 * Z);
            const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * bl;
            table.emplace_back(static_cast<float>(r / lum), static_cast<float>(g / lum), static_cast<float>(bl / lum));
        }
    }
    const double u = std::clamp((std::log(std::max(T, 1000.0)) - std::log(1000.0)) / (std::log(50000.0) - std::log(1000.0)), 0.0, 1.0) * 127.0;
    const int i = std::min(static_cast<int>(u), 126);
    const float f = static_cast<float>(u - i);
    return glm::mix(table[i], table[i + 1], f);
}

glm::dvec3 cubeDir(int face, double u, double v) {
    switch (face) {
        case 0: return glm::normalize(glm::dvec3(1.0, -v, -u));
        case 1: return glm::normalize(glm::dvec3(-1.0, -v, u));
        case 2: return glm::normalize(glm::dvec3(u, 1.0, v));
        case 3: return glm::normalize(glm::dvec3(u, -1.0, -v));
        case 4: return glm::normalize(glm::dvec3(u, -v, 1.0));
        default: return glm::normalize(glm::dvec3(-u, -v, -1.0));
    }
}

float atmosphereFor(universe::PlanetLook l) {
    switch (l) {
        case universe::PlanetLook::Earthlike: return 0.8f;
        case universe::PlanetLook::Cloudy: return 1.0f;
        case universe::PlanetLook::Desert: return 0.35f;
        case universe::PlanetLook::GasGiantWarm:
        case universe::PlanetLook::GasGiantCold: return 0.6f;
        case universe::PlanetLook::Ice: return 0.15f;
        case universe::PlanetLook::Ocean: return 0.8f;
        default: return 0.0f;
    }
}

uint64_t positionHash(const glm::dvec3& p) {
    const auto q = [](double v) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(v / 1e9))); };
    uint64_t h = q(p.x) * 0x9E3779B97F4A7C15ull;
    h ^= q(p.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= q(p.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return h;
}

// Stable catalogue-style name for an unnamed star, from its position.
std::string proceduralStarName(const glm::dvec3& p) {
    static const char* prefixes[] = {"HX", "KV", "TQ", "ZR", "LN", "OP"};
    const uint64_t h = positionHash(p);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s %04u-%03u", prefixes[h % 6], static_cast<unsigned>((h >> 8) % 10000u),
                  static_cast<unsigned>((h >> 24) % 1000u));
    return buf;
}

}  // namespace

// Invented universe: a deterministic planetary system around a star you are close to. Orbits
// follow Kepler's third law for the star's mass; each planet's type follows its equilibrium
// temperature (lava worlds close in, temperate ocean and Earth-like worlds, gas giants beyond the
// snow line, ice worlds far out).
void UniverseScene::appendPlanetarySystem(const universe::StarRecord& st, const std::string& starName, double simSeconds,
                                          std::vector<BodyDesc>& bodies) {
    using universe::PlanetLook;
    const double L = std::pow(10.0, (units::kSolarAbsMagV - st.absMag) / 2.5);
    if (L > 2e4 || L < 1e-5) return;                      // blue supergiants and dark remnants: no planets
    std::mt19937_64 rng(positionHash(st.pos) ^ 0xA5A5A5A55A5A5A5Aull);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int count = static_cast<int>(U(rng) * 7.0);
    if (count == 0) return;
    const double mStar = std::clamp(std::pow(L, 1.0 / 3.5), 0.08, 30.0);
    const double snowLine = 2.7 * std::sqrt(L);
    // Orientation of the system's orbital plane.
    std::normal_distribution<double> N(0.0, 1.0);
    const glm::dvec3 n = glm::normalize(glm::dvec3(N(rng), N(rng), N(rng)) + glm::dvec3(1e-9));
    const glm::dvec3 e1 = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    const glm::dvec3 e2 = glm::cross(n, e1);
    const glm::dmat3 frame(e1, e2, n);                    // system "ecliptic" -> galactic
    const double T = simSeconds / (36525.0 * units::kDay);
    const double Tn = T + 1.0 / 36525.0;
    double a = std::max(0.02, (0.25 + 0.4 * U(rng)) * std::sqrt(std::max(L, 0.01)));
    for (int p = 0; p < count; ++p) {
        if (p > 0) a *= 1.5 + 0.7 * U(rng);
        universe::OrbitElements el;
        el.aAU = a;
        el.e = 0.12 * U(rng) * U(rng);
        el.iDeg = 4.0 * (U(rng) - 0.5);
        el.LDeg = 360.0 * U(rng);
        el.varpiDeg = 360.0 * U(rng);
        el.OmegaDeg = 360.0 * U(rng);
        el.LRate = 36000.0 / (std::pow(a, 1.5) / std::sqrt(mStar));
        const double teq = 278.0 * std::pow(L, 0.25) / std::sqrt(a);
        BodyDesc b;
        b.name = starName + " " + static_cast<char>('b' + p);
        b.kind = BodyKind::Planet;
        double radiusKm;
        if (a > snowLine && U(rng) < 0.75) {
            b.look = teq > 110.0 ? PlanetLook::GasGiantWarm : PlanetLook::GasGiantCold;
            radiusKm = 22000.0 + 50000.0 * U(rng);
            if (U(rng) < 0.35) {
                b.ringInner = radiusKm * 1.25;
                b.ringOuter = radiusKm * (1.9 + 0.6 * U(rng));
            }
        } else if (teq > 900.0) {
            b.look = PlanetLook::Lava;
            radiusKm = 3000.0 + 6000.0 * U(rng);
        } else if (teq > 400.0) {
            b.look = U(rng) < 0.5 ? PlanetLook::Rocky : PlanetLook::Desert;
            radiusKm = 2500.0 + 6500.0 * U(rng);
        } else if (teq > 240.0) {
            b.look = U(rng) < 0.6 ? PlanetLook::Earthlike : PlanetLook::Ocean;
            radiusKm = 4500.0 + 5500.0 * U(rng);
        } else if (teq > 150.0) {
            b.look = U(rng) < 0.5 ? PlanetLook::Desert : PlanetLook::Cloudy;
            radiusKm = 3000.0 + 6000.0 * U(rng);
        } else {
            b.look = PlanetLook::Ice;
            radiusKm = 2000.0 + 7000.0 * U(rng);
        }
        b.tint = glm::vec3(0.85f + 0.3f * static_cast<float>(U(rng)), 0.85f + 0.3f * static_cast<float>(U(rng)),
                           0.85f + 0.3f * static_cast<float>(U(rng)));
        b.atmosphere = atmosphereFor(b.look);
        b.seed = static_cast<uint32_t>((positionHash(st.pos) >> 16) % 4096u) + static_cast<uint32_t>(p) * 97u;
        const glm::dvec3 g = st.pos + frame * (orbits::position(el, T) * units::kAU);
        const glm::dvec3 gn = st.pos + frame * (orbits::position(el, Tn) * units::kAU);
        b.pos = toRenderer(g);
        b.vel = (toRenderer(gn) - b.pos) / (units::kDay / units::secondsPerM(massSolar_));
        b.radius = radiusKm * 1e3 / metersPerM_;
        b.ringInner *= 1e3 / metersPerM_;
        b.ringOuter *= 1e3 / metersPerM_;
        b.axis = glm::vec3(glm::normalize(R_ * (frame * orbits::pole(el, T))));
        b.rotation = static_cast<float>(std::fmod(simSeconds / (20000.0 + 40000.0 * p), 2.0 * kPi));
        const glm::dvec3 toStar = st.pos - g;
        const double dAU = glm::length(toStar) / units::kAU;
        b.lightDir = glm::vec3(glm::normalize(R_ * toStar));
        b.lightIntensity = static_cast<float>(std::clamp(1.6 * std::pow(std::sqrt(L) / std::max(dAU, 0.01), 0.7), 0.05, 4.0));
        b.lightColor = blackbodyColor(st.teff);
        char buf[160];
        std::snprintf(buf, sizeof buf, "planet of %s, %.2f AU out, about %.0f K%s", starName.c_str(), a, teq,
                      b.ringOuter > 0.0 ? ", with rings" : "");
        b.info = buf;
        bodies.push_back(b);
    }
}

// CPU/GPU agreement of the galaxy model (BHS_GALAXY_SELFTEST=1): random points in five galaxies of
// every type, at several levels of detail, evaluated by galaxy_model.glsl and by GalaxyEval.
static void galaxySelfTest() {
#if !BHS_GALAXY_MODEL_STUB
    using namespace universe;
    const double kpc = 1e3 * units::kParsec;
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    std::vector<GalaxyModel> models(5);
    for (int i = 0; i < 5; ++i) {
        GalaxyModel& m = models[static_cast<size_t>(i)];
        m.type = static_cast<GalaxyType>(i);
        m.seed = 100u + static_cast<uint32_t>(i) * 7919u;
        m.center = glm::dvec3(U(rng), U(rng), U(rng)) * (50.0 * kpc);
        m.normal = glm::normalize(glm::dvec3(U(rng), U(rng), U(rng)));
        m.major = glm::normalize(glm::cross(m.normal, glm::dvec3(0.3, 0.8, 0.5)));
        m.dustExtinctionPerKpc = i == 3 ? 0.0 : 3.0;
        if (i == 1) m.barLengthPc = 3500.0;
        if (i == 4) m.armContrast = 1.3;
    }
    const glm::dvec3 view = glm::dvec3(1234.0, -567.0, 89.0) * units::kParsec;
    std::vector<GalaxyGpu> gpu;
    for (const GalaxyModel& m : models) gpu.push_back(packGalaxyGpu(m, view));
    struct Query {
        glm::vec4 pLod;
        glm::uvec4 galaxy;
    };
    const int n = 4096;
    std::vector<Query> q(n);
    const float lods[] = {0.0f, 0.03f, 0.3f, 2.0f};
    for (int i = 0; i < n; ++i) {
        const size_t g = static_cast<size_t>(i % 5);
        const double r = models[g].boundingRadiusPc() * units::kParsec * std::pow(10.0, -2.5 * (0.5 + 0.5 * U(rng)));
        glm::dvec3 d = glm::normalize(glm::dvec3(U(rng), U(rng), U(rng)));
        d -= 0.8 * glm::dot(d, models[g].normal) * models[g].normal;   // favour the disk
        const glm::dvec3 p = models[g].center + d * r - view;
        q[static_cast<size_t>(i)] = {glm::vec4(glm::vec3(p / kpc), lods[i % 4]), glm::uvec4(static_cast<unsigned>(g), 0, 0, 0)};
    }
    ShaderProgram prog;
    if (!prog.loadCompute("galaxy_selftest.comp")) {
        logError("galaxy model self-test: shader failed to compile");
        return;
    }
    GLuint buf[3];
    glGenBuffers(3, buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(gpu.size() * sizeof(GalaxyGpu)), gpu.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(q.size() * sizeof(Query)), q.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[2]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(n * 2 * sizeof(glm::vec4)), nullptr, GL_STATIC_READ);
    for (GLuint i = 0; i < 3; ++i) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buf[i]);
    prog.use();
    prog.set("uCount", n);
    glDispatchCompute(static_cast<GLuint>((n + 63) / 64), 1, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    std::vector<glm::vec4> res(static_cast<size_t>(n) * 2);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(res.size() * sizeof(glm::vec4)), res.data());
    glDeleteBuffers(3, buf);
    glUseProgram(0);
    // Relative differences, with a floor of 1e-4 of each quantity's largest value (near-zero values
    // at the edge of an HII knot or a dust cloud are not meaningful relative to themselves).
    std::vector<std::array<double, 6>> vals(static_cast<size_t>(n));
    double peak[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < n; ++i) {
        const Query& qi = q[static_cast<size_t>(i)];
        const detail::GalaxyEval ev(models[qi.galaxy.x]);
        detail::GalaxySample s;
        ev.sample(view + glm::dvec3(glm::vec3(qi.pLod)) * kpc, s, qi.pLod.w * 1000.0);
        const auto lum = [](const glm::dvec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; };
        const double cpu[3] = {lum(s.starLight), lum(s.nebula), s.tau};
        const glm::vec4 a = res[2 * static_cast<size_t>(i)], b = res[2 * static_cast<size_t>(i) + 1];
        const double gpuv[3] = {lum(glm::dvec3(a)), lum(glm::dvec3(b)), static_cast<double>(a.w)};
        for (int k = 0; k < 3; ++k) {
            vals[static_cast<size_t>(i)][static_cast<size_t>(k)] = cpu[k];
            vals[static_cast<size_t>(i)][static_cast<size_t>(k + 3)] = gpuv[k];
            peak[k] = std::max(peak[k], std::abs(cpu[k]));
        }
    }
    double worst[3] = {0.0, 0.0, 0.0};
    int bad = 0;
    for (const auto& v : vals)
        for (int k = 0; k < 3; ++k) {
            const double scale = std::max({std::abs(v[static_cast<size_t>(k)]), std::abs(v[static_cast<size_t>(k + 3)]), 1e-4 * peak[k], 1e-12});
            const double rel = std::abs(v[static_cast<size_t>(k)] - v[static_cast<size_t>(k + 3)]) / scale;
            worst[k] = std::max(worst[k], rel);
            if (rel > 0.02 || !std::isfinite(v[static_cast<size_t>(k + 3)])) ++bad;
        }
    if (bad)
        logWarn("galaxy model self-test: {} points, worst relative difference light {:.2e}, nebulae {:.2e}, dust {:.2e}, {} above 2%", n,
                worst[0], worst[1], worst[2], bad);
    else
        logInfo("galaxy model self-test: {} points, worst relative difference light {:.2e}, nebulae {:.2e}, dust {:.2e}", n, worst[0],
                worst[1], worst[2]);
#endif
}

// ---------------------------------------------------------------------------------------------
bool UniverseScene::init() {
    jd0_ = orbits::julianDateNow();
    bool ok = true;
    for (int pass = 0; pass < 3; ++pass) {
        ShaderProgram& p = pass == 0 ? countProg_ : (pass == 1 ? scanProg_ : scatterProg_);
        p.setDefines({"BHS_PASS " + std::to_string(pass)});
        ok = p.loadCompute("universe_stars.comp") && ok;
    }
    // The bake shader must match the CPU galaxy model that packs its constants.
    const bool haveModel = !BHS_GALAXY_MODEL_STUB && std::filesystem::exists(ShaderProgram::shaderDir() + "/galaxy_model.glsl");
    bakeOk_ = bakeProg_.loadCompute(haveModel ? "universe_bake.comp" : "universe_bake_fallback.comp");
    if (!haveModel) logWarn("built with the placeholder galaxy model: using the fallback galaxy model for the sky");
    if (haveModel && std::getenv("BHS_GALAXY_SELFTEST")) galaxySelfTest();
    glGenBuffers(1, &starBuf_);
    glGenBuffers(1, &sortedBuf_);
    glGenBuffers(1, &countBuf_);
    glGenBuffers(1, &offsetBuf_);
    glGenBuffers(1, &nearGalBuf_);
    glGenBuffers(1, &spriteBuf_);
    glGenBuffers(1, &spriteTileBuf_);
    glGenBuffers(1, &spriteIndexBuf_);
    const GLsizeiptr tiles = 6 * kTileGrid * kTileGrid;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, countBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, tiles * 4, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, offsetBuf_);
    std::vector<uint32_t> zeros(static_cast<size_t>(tiles) + 1, 0u);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (tiles + 1) * 4, zeros.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sortedBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(StarGpuOut), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenTextures(1, &cube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
    int levels = 1;
    if (const char* e = std::getenv("BHS_BAKE_RES")) bakeRes_ = std::clamp(std::atoi(e) / 256 * 256, 256, 2048);
    for (int r = bakeRes_; r > 1; r >>= 1) ++levels;
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, GL_RGBA16F, bakeRes_, bakeRes_);
    for (int l = 0; l < levels; ++l) glClearTexImage(cube_, l, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    if (!ok) logError("universe star shaders failed to compile");
    return ok;
}

void UniverseScene::shutdown() {
    for (ShaderProgram* p : {&countProg_, &scanProg_, &scatterProg_, &bakeProg_}) p->destroy();
    for (GLuint* b : {&starBuf_, &sortedBuf_, &countBuf_, &offsetBuf_, &nearGalBuf_, &spriteBuf_, &spriteTileBuf_, &spriteIndexBuf_}) {
        if (*b) glDeleteBuffers(1, b);
        *b = 0;
    }
    if (cube_) glDeleteTextures(1, &cube_);
    cube_ = 0;
}

bool UniverseScene::configure(int mode, uint32_t seed, int placement, const std::string& assetDir, std::string* error) {
    active_ = false;
    mode_ = mode;
    stars_.clear();
    starCount_ = 0;
    targets_.clear();
    if (mode == 0) {
        content_ = universe::UniverseContent{};
        status_ = "Painted backdrop";
        return true;
    }
    const double t0 = glfwGetTime();
    if (mode == 2) {
        std::string err;
        if (!universe::buildRealUniverse(content_, assetDir, static_cast<universe::BlackHolePlacement>(placement), &err)) {
            status_ = "Real universe unavailable: " + err;
            if (error) *error = err;
            return false;
        }
    } else {
        universe::buildInventedUniverse(content_, seed);
    }
    seed_ = seed;
    field_.configure(content_, seed);
    massSolar_ = 0.0;              // forces a mapping rebuild on the next update
    lastProjectCam_ = glm::dvec3(1e300);
    bakeCam_ = glm::dvec3(1e300);
    bakeStep_ = 1 << 30;
    galDistAt_ = glm::dvec3(1e300);
    active_ = true;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s universe: %zu catalogue stars, %zu galaxies (built in %.0f ms)", mode == 2 ? "Real" : "Invented",
                  content_.stars.size(), content_.galaxies.size(), (glfwGetTime() - t0) * 1000.0);
    status_ = buf;
    logInfo("{}", status_);
    return true;
}

void UniverseScene::rebuildMapping(double bhMassSolar) {
    massSolar_ = bhMassSolar;
    metersPerM_ = units::metersPerM(bhMassSolar);
    const glm::dvec3 s = glm::normalize(content_.blackHoleSpinAxis);
    glm::dvec3 x, z;
    const glm::dvec3 nodes = glm::cross(s, glm::normalize(content_.host.normal));
    if (glm::length(nodes) > 0.05) {
        // Renderer -Z (where the default views look) runs along the line where the accretion disk's
        // plane meets the galaxy's, pointing towards the galactic centre when it can: the galaxy's band
        // then crosses behind the black hole, tilted against the disk.
        z = glm::normalize(nodes);
        const glm::dvec3 toCentre = content_.host.center - content_.blackHolePos;
        if (glm::dot(-z, toCentre) < 0.0) z = -z;
        x = glm::cross(s, z);
    } else {
        glm::dvec3 ref(1.0, 0.0, 0.0);
        if (std::abs(glm::dot(ref, s)) > 0.9) ref = glm::dvec3(0.0, 0.0, 1.0);
        x = glm::normalize(ref - s * glm::dot(ref, s));
        z = glm::cross(x, s);
    }
    R_ = glm::transpose(glm::dmat3(x, s, z));   // rows: renderer axes in galactic coordinates
}

glm::dvec3 UniverseScene::toRenderer(const glm::dvec3& g) const { return R_ * (g - content_.blackHolePos) / metersPerM_; }
glm::dvec3 UniverseScene::toGalactic(const glm::dvec3& r) const { return content_.blackHolePos + glm::transpose(R_) * (r * metersPerM_); }
glm::dvec3 UniverseScene::dirToRenderer(const glm::dvec3& d) const { return R_ * d; }

void UniverseScene::uploadStars(const glm::dvec3& origin) {
    starOrigin_ = origin;
    std::vector<StarGpuIn> data;
    data.reserve(stars_.size());
    for (const universe::StarRecord& s : stars_) {
        const glm::dvec3 rel = (s.pos - origin) / units::kParsec;
        data.push_back({glm::vec4(glm::vec3(rel), s.absMag), glm::vec4(blackbodyColor(s.teff), s.radiusSolar)});
    }
    starCount_ = data.size();
    const size_t n = std::max<size_t>(data.size(), 1);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, starBuf_);
    if (n > starCapacity_) {
        starCapacity_ = n + n / 4;
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(starCapacity_ * sizeof(StarGpuIn)), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sortedBuf_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(starCapacity_ * sizeof(StarGpuOut)), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, starBuf_);
    }
    if (!data.empty()) glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(data.size() * sizeof(StarGpuIn)), data.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void UniverseScene::projectStars(const glm::dvec3& camGal, float limitingMag) {
    if (!countProg_.valid() || !scanProg_.valid() || !scatterProg_.valid()) return;
    const glm::vec3 camPc = glm::vec3((camGal - starOrigin_) / units::kParsec);
    const glm::mat3 R = glm::mat3(R_);
    const int n = static_cast<int>(starCount_);
    const uint32_t zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, countBuf_);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, starBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, countBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, offsetBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sortedBuf_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
    for (ShaderProgram* p : {&countProg_, &scanProg_, &scatterProg_}) {
        p->use();
        p->set("uCount", n);
        p->set("uCamPc", camPc);
        p->set("uR", R);
        p->set("uLimMag", limitingMag);
        p->set("uT", kTileGrid);
        p->set("uDust", 0);
        p->set("uHaveDust", bakeOk_ ? 1 : 0);
        if (p == &scanProg_) glDispatchCompute(1, 1, 1);
        else if (n > 0) glDispatchCompute(static_cast<GLuint>((n + 255) / 256), 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    glUseProgram(0);
    if (std::getenv("BHS_DEBUG_STARS")) {   // one-off check of the GPU culling and binning
        uint32_t total = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, offsetBuf_);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(6 * kTileGrid * kTileGrid) * 4, 4, &total);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        logInfo("universe stars: {} uploaded, {} brighter than mag {}", n, total, limitingMag);
    }
}

void UniverseScene::buildGalaxyLists(const glm::dvec3& camGal) {
    // Galaxies that cover more than ~1 degree are ray-marched as volumes: the host plus the 15 largest
    // on the sky, marched front to back so nearer dust dims farther light.
    struct NearGalaxy {
        double angular, key;
        const universe::GalaxyModel* model;
    };
    std::vector<NearGalaxy> near;
    near.push_back({1e9, glm::length(content_.host.center - camGal) - content_.host.boundingRadiusPc() * units::kParsec, &content_.host});
    std::vector<SpriteGpu> sprites;
    sprites.reserve(content_.galaxies.size());
    const double texel = 1.5708 / bakeRes_ * 2.0;
    for (const universe::GalaxyRecord& gr : content_.galaxies) {
        const universe::GalaxyModel& m = gr.model;
        const glm::dvec3 rel = m.center - camGal;
        const double d = glm::length(rel);
        const double rb = m.boundingRadiusPc() * units::kParsec;
        if (d < 1.0) continue;
        if (rb / d > 0.02) {
            near.push_back({rb / d, d - rb, &m});
            continue;
        }
        const glm::dvec3 dirG = rel / d;
        const glm::dvec3 dirR = glm::normalize(R_ * dirG);
        const double rVis = 0.45 * rb;
        double theta = std::atan(rVis / d);
        const double dPc = d / units::kParsec;
        const double flux = m.luminositySolar / (4.0 * kPi * dPc * dPc);   // L_sun / pc^2
        double peak = kSurfaceToHdr * flux / (0.25 * kPi * theta * theta + 1e-300);
        // Deep-sky exposure, like a long-exposure photograph: distant galaxies are brightened (setting,
        // 12x by default) so the sky fills with faint galaxies; 1x where galaxies hand over to the
        // volumetric renderer, so nothing jumps.
        peak *= 1.0 + (std::max(static_cast<double>(deepSky_), 1.0) - 1.0) * (1.0 - glm::smoothstep(0.004, 0.02, rb / d));
        if (theta < texel) {   // keep tiny galaxies one texel wide, conserving their flux
            peak *= (theta * theta) / (texel * texel);
            theta = texel;
        }
        if (peak < 1e-7) continue;
        const glm::dvec3 nR = R_ * m.normal;
        glm::dvec3 major = glm::cross(dirR, nR);
        if (glm::length(major) < 1e-9) major = glm::cross(dirR, std::abs(dirR.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        major = glm::normalize(major);
        const bool ell = m.type == universe::GalaxyType::Elliptical || m.type == universe::GalaxyType::Lenticular;
        const double q = ell ? m.ellipticalAxisRatio : std::max(std::abs(glm::dot(nR, dirR)), 0.12);
        const float type = ell ? 0.0f : (m.type == universe::GalaxyType::BarredSpiral ? 2.0f : 1.0f);
        SpriteGpu s;
        s.dirCos = glm::vec4(glm::vec3(dirR), static_cast<float>(std::cos(theta)));
        s.axisQ = glm::vec4(glm::vec3(major), static_cast<float>(q));
        s.color = glm::vec4(ell ? m.bulgeColor : m.diskColor, static_cast<float>(std::min(peak, 1e4)));
        s.params = glm::vec4(type, static_cast<float>(std::max(m.armCount, 2)), static_cast<float>(m.armPitchDeg * kPi / 180.0 * 1.6),
                             static_cast<float>(m.seed % 997u));
        sprites.push_back(s);
    }
    if (near.size() > static_cast<size_t>(kMaxNearGalaxies)) {
        std::partial_sort(near.begin(), near.begin() + kMaxNearGalaxies, near.end(),
                          [](const NearGalaxy& a, const NearGalaxy& b) { return a.angular > b.angular; });
        near.resize(kMaxNearGalaxies);   // the dropped ones are still too big for sprites: skip them
    }
    std::sort(near.begin(), near.end(), [](const NearGalaxy& a, const NearGalaxy& b) { return a.key < b.key; });
    std::vector<universe::GalaxyGpu> nearGpu;
    for (const NearGalaxy& n : near) nearGpu.push_back(universe::packGalaxyGpu(*n.model, camGal));
    nearGalCount_ = static_cast<int>(nearGpu.size());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, nearGalBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(nearGpu.size() * sizeof(universe::GalaxyGpu)), nearGpu.data(), GL_DYNAMIC_DRAW);

    // Bin sprites into face tiles by cone overlap.
    const int T = kSpriteGrid, tiles = 6 * T * T;
    static std::vector<glm::dvec3> tileCenter;
    static std::vector<double> tileRadius;
    if (tileCenter.empty()) {
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < T; ++j)
                for (int i = 0; i < T; ++i) {
                    const double u0 = 2.0 * i / T - 1.0, u1 = 2.0 * (i + 1) / T - 1.0, v0 = 2.0 * j / T - 1.0, v1 = 2.0 * (j + 1) / T - 1.0;
                    const glm::dvec3 c = cubeDir(f, 0.5 * (u0 + u1), 0.5 * (v0 + v1));
                    double r = 0.0;
                    for (const glm::dvec3& corner : {cubeDir(f, u0, v0), cubeDir(f, u1, v0), cubeDir(f, u0, v1), cubeDir(f, u1, v1)})
                        r = std::max(r, std::acos(std::clamp(glm::dot(c, corner), -1.0, 1.0)));
                    tileCenter.push_back(c);
                    tileRadius.push_back(r);
                }
    }
    std::vector<std::vector<uint32_t>> lists(static_cast<size_t>(tiles));
    for (size_t si = 0; si < sprites.size(); ++si) {
        const glm::dvec3 d(sprites[si].dirCos);
        const double th = std::acos(std::clamp(static_cast<double>(sprites[si].dirCos.w), -1.0, 1.0));
        for (int t = 0; t < tiles; ++t) {
            const double ang = std::acos(std::clamp(glm::dot(d, tileCenter[static_cast<size_t>(t)]), -1.0, 1.0));
            if (ang < th + tileRadius[static_cast<size_t>(t)]) lists[static_cast<size_t>(t)].push_back(static_cast<uint32_t>(si));
        }
    }
    std::vector<uint32_t> ranges(static_cast<size_t>(tiles) * 2), indices;
    for (int t = 0; t < tiles; ++t) {
        ranges[static_cast<size_t>(t) * 2] = static_cast<uint32_t>(indices.size());
        ranges[static_cast<size_t>(t) * 2 + 1] = static_cast<uint32_t>(lists[static_cast<size_t>(t)].size());
        indices.insert(indices.end(), lists[static_cast<size_t>(t)].begin(), lists[static_cast<size_t>(t)].end());
    }
    if (sprites.empty()) sprites.push_back(SpriteGpu{});
    if (indices.empty()) indices.push_back(0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, spriteBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(sprites.size() * sizeof(SpriteGpu)), sprites.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, spriteTileBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(ranges.size() * 4), ranges.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, spriteIndexBuf_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(indices.size() * 4), indices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void UniverseScene::bakeChunk(int step) {
    if (!bakeOk_) return;
    const int chunks = std::max(1, bakeRes_ / kBakeChunkRows);
    const int face = step / chunks, rows = bakeRes_ / chunks, row0 = (step % chunks) * rows;
    bakeProg_.use();
    bakeProg_.set("uRes", bakeRes_);
    bakeProg_.set("uFace", face);
    bakeProg_.set("uRowOffset", row0);
    bakeProg_.set("uRt", glm::mat3(glm::transpose(R_)));
    bakeProg_.set("uNearCount", nearGalCount_);
    bakeProg_.set("uScale", static_cast<float>(kSurfaceToHdr / (4.0 * kPi)));
    bakeProg_.set("uResolvedPc", 300.0f);
    static const float dustScale = std::getenv("BHS_DUST_SCALE") ? static_cast<float>(std::atof(std::getenv("BHS_DUST_SCALE"))) : 1.0f;
    bakeProg_.set("uDustScale", dustScale);
    bakeProg_.set("uSpriteGrid", kSpriteGrid);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, nearGalBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, spriteBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, spriteTileBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, spriteIndexBuf_);
    glBindImageTexture(0, cube_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    static const bool debugBake = std::getenv("BHS_DEBUG_BAKE") != nullptr;
    const double tb = debugBake ? (glFinish(), glfwGetTime()) : 0.0;
    glDispatchCompute(static_cast<GLuint>(bakeRes_ / 16), static_cast<GLuint>(rows / 16), 1);
    if (debugBake) {
        glFinish();
        logInfo("sky bake face {} rows {}-{}: {:.2f} ms ({} near galaxies)", face, row0, row0 + rows, (glfwGetTime() - tb) * 1000.0, nearGalCount_);
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glUseProgram(0);
    if (step == bakeSteps() - 1) {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        // Mean sky luminance from the 1x1 level of each face (six texels: a negligible read-back).
        int top = 0;
        for (int r = bakeRes_; r > 1; r >>= 1) ++top;
        double sum = 0.0;
        for (int f = 0; f < 6; ++f) {
            float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, top, GL_RGBA, GL_FLOAT, px);
            sum += 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
        }
        skyMean_ = sum / 6.0;
        // Keep the average sky below a dark-sky level (a band in a dark sky, as from inside a disk).
        constexpr double kSkyTarget = 0.035;
        skyExposureTarget_ = skyMean_ > kSkyTarget ? kSkyTarget / skyMean_ : 1.0;
        if (std::getenv("BHS_DEBUG_BAKE")) logInfo("sky mean luminance {:.4f}, exposure {:.3f}", skyMean_, skyExposureTarget_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
}

void UniverseScene::refreshGalaxyDistances(const glm::dvec3& camGal) {
    if (glm::length(camGal - galDistAt_) < 0.01 * std::min(galSurfaceM_ > 0.0 ? galSurfaceM_ : galCentreM_, galCentreM_)) return;
    galDistAt_ = camGal;
    galCentreM_ = glm::length(camGal - content_.host.center);
    galSurfaceM_ = 1e300;
    for (const universe::GalaxyRecord& g : content_.galaxies) {
        const double dc = glm::length(g.model.center - camGal);
        galCentreM_ = std::min(galCentreM_, dc);
        const double ds = dc - g.model.boundingRadiusPc() * units::kParsec;
        if (ds > 0.0) galSurfaceM_ = std::min(galSurfaceM_, ds);
    }
}

void UniverseScene::buildTargets(double simSeconds) {
    targets_.clear();
    auto nameOf = [&](int32_t i) -> std::string {
        return (i >= 0 && i < static_cast<int32_t>(content_.names.size())) ? content_.names[static_cast<size_t>(i)] : std::string();
    };
    for (const universe::StarRecord& s : content_.stars) {
        const std::string n = nameOf(s.name);
        if (n.empty()) continue;
        UniverseTarget t;
        t.name = n;
        t.pos = toRenderer(s.pos);
        t.radius = s.radiusSolar * units::kSolarRadius / metersPerM_;
        t.kind = 0;
        char buf[128];
        std::snprintf(buf, sizeof buf, "star, %.0f K, absolute magnitude %.1f", s.teff, s.absMag);
        t.info = buf;
        t.absMag = s.absMag;
        targets_.push_back(t);
    }
    // The home galaxy as a whole, seen from outside.
    {
        UniverseTarget t;
        t.name = content_.mode == universe::Mode::Real ? "Milky Way, seen from outside" : "Home galaxy, seen from outside";
        t.pos = toRenderer(content_.host.center);
        t.radius = 0.55 * content_.host.boundingRadiusPc() * units::kParsec / metersPerM_;
        t.kind = 1;
        t.info = "the whole galaxy around the black hole";
        t.axis = glm::normalize(R_ * content_.host.normal);
        targets_.push_back(t);
    }
    for (const universe::GalaxyRecord& g : content_.galaxies) {
        const std::string n = nameOf(g.name);
        if (n.empty()) continue;
        UniverseTarget t;
        t.name = n;
        t.pos = toRenderer(g.model.center);
        t.radius = 0.5 * g.model.boundingRadiusPc() * units::kParsec / metersPerM_;
        t.kind = 1;
        t.info = "galaxy";
        if (g.model.type != universe::GalaxyType::Elliptical) t.axis = glm::normalize(R_ * g.model.normal);
        targets_.push_back(t);
    }
    for (const universe::NamedPlace& p : content_.places)
        targets_.push_back({p.name, toRenderer(p.pos), std::max(p.radiusMeters, 1.0) / metersPerM_, 2, "place"});
    (void)simSeconds;
}

void UniverseScene::update(const glm::dvec3& camPosM, double simSeconds, const Settings& s, std::vector<BodyDesc>& bodies) {
    if (!active_) return;
    const bool remap = s.bhMassSolar != massSolar_;
    if (remap) rebuildMapping(s.bhMassSolar);
    const glm::dvec3 camGal = toGalactic(camPosM);
    if (s.deepSky != deepSky_) {   // re-bake with the new galaxy brightness
        deepSky_ = s.deepSky;
        bakeDirty_ = true;
    }

    // ---- stars: explicit + procedural (+ the black hole itself as a point of light from afar)
    const double t0 = glfwGetTime();
    bool changed = field_.gather(camGal, kLimitingMag, kMaxProcStars, proc_) || remap || stars_.empty();
    const bool farFromHole = glm::length(camPosM) > 2e4;
    if (farFromHole != holeShown_) changed = true;
    if (changed) {
        holeShown_ = farFromHole;
        stars_ = content_.stars;
        stars_.insert(stars_.end(), proc_.begin(), proc_.end());
        if (farFromHole) {
            universe::StarRecord h;
            h.pos = content_.blackHolePos;
            h.absMag = -9.0f;
            h.teff = 7000.0f;
            h.radiusSolar = 1.0f;
            stars_.push_back(h);
        }
    }
    starsChanged_ = starsChanged_ || changed;
    gatherMs_ = (glfwGetTime() - t0) * 1000.0;

    // ---- nearest stars: discs when close, and the flight-speed scale
    nearestSurfaceM_ = 1e300;
    nearestName_.clear();
    struct Near {
        double d;
        size_t i;
    };
    Near best[8];
    int nb = 0;
    for (size_t i = 0; i < stars_.size(); ++i) {
        const double d = glm::length(stars_[i].pos - camGal);
        if (nb < 8 || d < best[nb - 1].d) {
            int k = std::min(nb, 7);
            while (k > 0 && best[k - 1].d > d) {
                best[k] = best[k - 1];
                --k;
            }
            best[k] = {d, i};
            nb = std::min(nb + 1, 8);
        }
    }
    for (int k = 0; k < nb; ++k) {
        const universe::StarRecord& st = stars_[best[k].i];
        const double rM = st.radiusSolar * units::kSolarRadius / metersPerM_;
        const double dM = best[k].d / metersPerM_;
        const bool isHole = glm::length(st.pos - content_.blackHolePos) < 1.0;
        if (isHole) continue;
        const bool named = st.name >= 0 && st.name < static_cast<int32_t>(content_.names.size());
        const std::string starName = named ? content_.names[static_cast<size_t>(st.name)] : proceduralStarName(st.pos);
        if (dM - rM < nearestSurfaceM_) {
            nearestSurfaceM_ = dM - rM;
            nearestName_ = starName;
            nearestStarPos_ = toRenderer(st.pos);
            nearestStarRadius_ = rM;
            nearestStarName_ = starName;
        }
        // Invented universe: the stars you get close to have planets.
        if (mode_ == 1 && k < 3 && best[k].d < 0.05 * units::kParsec) {
            const size_t before = bodies.size();
            appendPlanetarySystem(st, starName, simSeconds, bodies);
            if (std::getenv("BHS_DEBUG_BODIES") && bodies.size() != before) {
                static std::string logged;
                if (logged != starName) {
                    logged = starName;
                    for (size_t q = before; q < bodies.size(); ++q) logInfo("planet {}: {}", bodies[q].name, bodies[q].info);
                }
            }
        }
        if (rM / dM < 2e-5) continue;   // not yet a visible disc
        BodyDesc b;
        b.name = starName;
        b.kind = BodyKind::Star;
        b.pos = toRenderer(st.pos);
        b.radius = rM;
        b.teff = st.teff;
        b.lightIntensity = 1.5f;   // bright enough to glow, dim enough to keep the colour and the detail
        b.atmosphere = static_cast<float>(std::clamp(70.0 / std::sqrt(std::max(static_cast<double>(st.radiusSolar), 1e-3)), 5.0, 90.0));
        b.seed = static_cast<uint32_t>(best[k].i % 4096u);
        b.info = "star";
        bodies.push_back(b);
    }
    refreshGalaxyDistances(camGal);
    if (galSurfaceM_ / metersPerM_ < nearestSurfaceM_) {
        nearestSurfaceM_ = galSurfaceM_ / metersPerM_;
        nearestName_ = "a galaxy";
    }

    // ---- solar system (real universe)
    if (!content_.bodies.empty()) {
        const double T = (julianDate(simSeconds) - 2451545.0) / 36525.0;
        const double Tn = T + 1.0 / 36525.0;   // one day later, for velocities
        for (const universe::OrbitingBody& ob : content_.bodies) {
            if (ob.hostStar < 0 || ob.hostStar >= static_cast<int32_t>(content_.stars.size())) continue;
            const universe::StarRecord& host = content_.stars[static_cast<size_t>(ob.hostStar)];
            const glm::dvec3 g = host.pos + ob.orbitFrame * (orbits::position(ob.elements, T) * units::kAU);
            const glm::dvec3 gn = host.pos + ob.orbitFrame * (orbits::position(ob.elements, Tn) * units::kAU);
            BodyDesc b;
            b.name = ob.name;
            b.kind = BodyKind::Planet;
            b.pos = toRenderer(g);
            b.vel = (toRenderer(gn) - b.pos) / (units::kDay / units::secondsPerM(s.bhMassSolar));
            b.radius = ob.radiusKm * 1e3 / metersPerM_;
            b.look = ob.look;
            b.seed = static_cast<uint32_t>(std::hash<std::string>{}(ob.name) % 4096u);
            b.tint = ob.tint;
            b.atmosphere = atmosphereFor(ob.look);
            glm::dvec3 pole = ob.orbitFrame * orbits::pole(ob.elements, T);
            if (ob.name == "Saturn") {   // Saturn's rings follow its own pole (IAU: RA 40.589, Dec 83.537)
                const double ra = 40.589 * kPi / 180.0, de = 83.537 * kPi / 180.0;
                const glm::dvec3 eq(std::cos(de) * std::cos(ra), std::cos(de) * std::sin(ra), std::sin(de));
                pole = ob.orbitFrame * orbits::equatorialToEcliptic(eq);
            }
            b.axis = glm::vec3(glm::normalize(R_ * pole));
            b.rotation = static_cast<float>(std::fmod(simSeconds / 36000.0, 2.0 * kPi));
            const glm::dvec3 toStar = host.pos - g;
            const double dAU = glm::length(toStar) / units::kAU;
            b.lightDir = glm::vec3(glm::normalize(R_ * toStar));
            b.lightIntensity = static_cast<float>(std::clamp(1.6 * std::pow(1.0 / std::max(dAU, 0.05), 0.7), 0.05, 4.0));
            b.lightColor = blackbodyColor(host.teff);
            if (ob.rings) {
                b.ringInner = ob.ringInnerKm * 1e3 / metersPerM_;
                b.ringOuter = ob.ringOuterKm * 1e3 / metersPerM_;
            }
            char buf[160];
            std::snprintf(buf, sizeof buf, "planet, %.0f km across, %.2f AU from its star", 2.0 * ob.radiusKm, dAU);
            b.info = buf;
            bodies.push_back(b);
        }
    }

    if (remap || !targetsBuilt_) {
        buildTargets(simSeconds);
        targetsBuilt_ = true;
    }
    remapped_ = remapped_ || remap;
}

void UniverseScene::prepareRender(const glm::dvec3& camPosM) {
    if (!active_) return;
    const glm::dvec3 camGal = toGalactic(camPosM);
    const bool remap = remapped_;
    remapped_ = false;
    // Stars: re-centre the single-precision upload near the camera, then project and bin.
    const bool recentre = glm::length(camGal - starOrigin_) > 50.0 * units::kParsec;
    if (starsChanged_ || recentre) uploadStars(camGal);
    if (starsChanged_ || recentre || remap || camGal != lastProjectCam_) {
        projectStars(camGal, kLimitingMag);
        lastProjectCam_ = camGal;
    }
    starsChanged_ = false;

    // Ease the sky exposure towards its target (about a second).
    skyExposure_ += (skyExposureTarget_ - skyExposure_) * 0.05;
    if (std::abs(skyExposure_ - skyExposureTarget_) < 1e-4) skyExposure_ = skyExposureTarget_;

    // ---- sky bake from the viewpoint, one strip of a face per frame while stale
    refreshGalaxyDistances(camGal);
    const double dGal = galCentreM_;
    // A full cycle (every strip of the six faces) always completes with the viewpoint and galaxy lists
    // captured at its start; if the camera moved meanwhile, another cycle follows.
    const double thr = std::max(0.5 * units::kParsec, 0.004 * dGal);
    if (glm::length(camGal - bakeCam_) > thr || remap) bakeDirty_ = true;
    if (bakeStep_ >= bakeSteps() && bakeDirty_) bakeStep_ = 0;
    if (bakeStep_ == 0) {
        bakeCam_ = camGal;
        buildGalaxyLists(camGal);
        bakeDirty_ = false;
    }
    if (bakeStep_ < bakeSteps()) bakeChunk(bakeStep_++);
}

}  // namespace bhs
