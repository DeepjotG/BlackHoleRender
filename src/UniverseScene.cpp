#include "UniverseScene.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>

#include "Log.h"
#include "Settings.h"
#include "Units.h"

namespace bhs {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Surface brightness (L_sun / pc^2 / sr) -> HDR radiance. Calibrated so the Milky Way band seen
// from the Sun has about the brightness of the painted backdrop's band.
constexpr double kSurfaceToHdr = 0.004;
constexpr float kLimitingMag = 9.0f;
constexpr size_t kMaxProcStars = 250000;
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

// Heliocentric ecliptic J2000 position (AU) from JPL-style elements at T Julian centuries.
glm::dvec3 keplerPosition(const universe::OrbitElements& el, double T) {
    const double d2r = kPi / 180.0;
    const double a = el.aAU + el.aRate * T, e = std::clamp(el.e + el.eRate * T, 0.0, 0.99);
    const double I = (el.iDeg + el.iRate * T) * d2r, L = (el.LDeg + el.LRate * T) * d2r;
    const double varpi = (el.varpiDeg + el.varpiRate * T) * d2r, Om = (el.OmegaDeg + el.OmegaRate * T) * d2r;
    const double w = varpi - Om;
    double M = std::remainder(L - varpi, 2.0 * kPi);
    double E = M + e * std::sin(M);
    for (int i = 0; i < 12; ++i) E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
    const double xp = a * (std::cos(E) - e), yp = a * std::sqrt(1.0 - e * e) * std::sin(E);
    const double cw = std::cos(w), sw = std::sin(w), cO = std::cos(Om), sO = std::sin(Om), cI = std::cos(I), sI = std::sin(I);
    return {(cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
            (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
            (sw * sI) * xp + (cw * sI) * yp};
}

glm::dvec3 orbitPoleEcliptic(const universe::OrbitElements& el, double T) {
    const double d2r = kPi / 180.0;
    const double I = (el.iDeg + el.iRate * T) * d2r, Om = (el.OmegaDeg + el.OmegaRate * T) * d2r;
    return {std::sin(I) * std::sin(Om), -std::sin(I) * std::cos(Om), std::cos(I)};
}

double julianDateNow() {
    using namespace std::chrono;
    const double unix = duration<double>(system_clock::now().time_since_epoch()).count();
    return 2440587.5 + unix / units::kDay;
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

}  // namespace

// ---------------------------------------------------------------------------------------------
bool UniverseScene::init() {
    bool ok = true;
    for (int pass = 0; pass < 3; ++pass) {
        ShaderProgram& p = pass == 0 ? countProg_ : (pass == 1 ? scanProg_ : scatterProg_);
        p.setDefines({"BHS_PASS " + std::to_string(pass)});
        ok = p.loadCompute("universe_stars.comp") && ok;
    }
    const bool haveModel = std::filesystem::exists(ShaderProgram::shaderDir() + "/galaxy_model.glsl");
    bakeOk_ = bakeProg_.loadCompute(haveModel ? "universe_bake.comp" : "universe_bake_fallback.comp");
    if (!haveModel) logWarn("galaxy_model.glsl not present yet: using the fallback galaxy model for the sky");
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
    for (int r = kBakeRes; r > 1; r >>= 1) ++levels;
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, GL_RGBA16F, kBakeRes, kBakeRes);
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
    bakeFace_ = 6;
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
    glm::dvec3 ref(1.0, 0.0, 0.0);
    if (std::abs(glm::dot(ref, s)) > 0.9) ref = glm::dvec3(0.0, 0.0, 1.0);
    const glm::dvec3 x = glm::normalize(ref - s * glm::dot(ref, s));
    const glm::dvec3 z = glm::cross(x, s);
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
    std::vector<universe::GalaxyGpu> nearGpu;
    nearGpu.push_back(universe::packGalaxyGpu(content_.host, camGal));
    std::vector<SpriteGpu> sprites;
    sprites.reserve(content_.galaxies.size());
    const double texel = 1.5708 / kBakeRes * 2.0;
    for (const universe::GalaxyRecord& gr : content_.galaxies) {
        const universe::GalaxyModel& m = gr.model;
        const glm::dvec3 rel = m.center - camGal;
        const double d = glm::length(rel);
        const double rb = m.boundingRadiusPc() * units::kParsec;
        if (d < 1.0) continue;
        if (rb / d > 0.07 && static_cast<int>(nearGpu.size()) < kMaxNearGalaxies) {
            nearGpu.push_back(universe::packGalaxyGpu(m, camGal));
            continue;
        }
        const glm::dvec3 dirG = rel / d;
        const glm::dvec3 dirR = glm::normalize(R_ * dirG);
        const double rVis = 0.45 * rb;
        double theta = std::atan(rVis / d);
        const double dPc = d / units::kParsec;
        const double flux = m.luminositySolar / (4.0 * kPi * dPc * dPc);   // L_sun / pc^2
        double peak = kSurfaceToHdr * flux / (0.25 * kPi * theta * theta + 1e-300);
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

void UniverseScene::bakeFace(int face, const glm::dvec3&) {
    if (!bakeOk_) return;
    bakeProg_.use();
    bakeProg_.set("uRes", kBakeRes);
    bakeProg_.set("uFace", face);
    bakeProg_.set("uRt", glm::mat3(glm::transpose(R_)));
    bakeProg_.set("uNearCount", nearGalCount_);
    bakeProg_.set("uScale", static_cast<float>(kSurfaceToHdr / (4.0 * kPi)));
    bakeProg_.set("uResolvedPc", 1500.0f);
    bakeProg_.set("uSpriteGrid", kSpriteGrid);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, nearGalBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, spriteBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, spriteTileBuf_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, spriteIndexBuf_);
    glBindImageTexture(0, cube_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(static_cast<GLuint>(kBakeRes / 16), static_cast<GLuint>(kBakeRes / 16), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glUseProgram(0);
    if (face == 5) {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
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
        targets_.push_back(t);
    }
    for (const universe::GalaxyRecord& g : content_.galaxies) {
        const std::string n = nameOf(g.name);
        if (n.empty()) continue;
        targets_.push_back({n, toRenderer(g.model.center), 0.5 * g.model.boundingRadiusPc() * units::kParsec / metersPerM_, 1, "galaxy"});
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
        const std::string starName = named ? content_.names[static_cast<size_t>(st.name)] : std::string("Star");
        if (dM - rM < nearestSurfaceM_) {
            nearestSurfaceM_ = dM - rM;
            nearestName_ = named ? starName : std::string("a star");
        }
        if (rM / dM < 2e-5) continue;   // not yet a visible disc
        BodyDesc b;
        b.name = starName;
        b.kind = BodyKind::Star;
        b.pos = toRenderer(st.pos);
        b.radius = rM;
        b.teff = st.teff;
        b.lightIntensity = 6.0f;
        b.seed = static_cast<uint32_t>(best[k].i % 4096u);
        b.info = "star";
        bodies.push_back(b);
    }
    for (const universe::GalaxyRecord& g : content_.galaxies) {
        const double d = (glm::length(g.model.center - camGal) - g.model.boundingRadiusPc() * units::kParsec) / metersPerM_;
        if (d > 0.0 && d < nearestSurfaceM_) {
            nearestSurfaceM_ = d;
            nearestName_ = "a galaxy";
        }
    }

    // ---- solar system (real universe)
    if (!content_.bodies.empty()) {
        static const double jd0 = julianDateNow();
        const double T = (jd0 + simSeconds / units::kDay - 2451545.0) / 36525.0;
        const double Tn = T + 1.0 / 36525.0;   // one day later, for velocities
        for (const universe::OrbitingBody& ob : content_.bodies) {
            if (ob.hostStar < 0 || ob.hostStar >= static_cast<int32_t>(content_.stars.size())) continue;
            const universe::StarRecord& host = content_.stars[static_cast<size_t>(ob.hostStar)];
            const glm::dvec3 g = host.pos + ob.orbitFrame * (keplerPosition(ob.elements, T) * units::kAU);
            const glm::dvec3 gn = host.pos + ob.orbitFrame * (keplerPosition(ob.elements, Tn) * units::kAU);
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
            glm::dvec3 pole = ob.orbitFrame * orbitPoleEcliptic(ob.elements, T);
            if (ob.name == "Saturn") {   // Saturn's rings follow its own pole (IAU: RA 40.589, Dec 83.537)
                const double ra = 40.589 * kPi / 180.0, de = 83.537 * kPi / 180.0, eps = 23.43928 * kPi / 180.0;
                const glm::dvec3 eq(std::cos(de) * std::cos(ra), std::cos(de) * std::sin(ra), std::sin(de));
                const glm::dvec3 ecl(eq.x, std::cos(eps) * eq.y + std::sin(eps) * eq.z, -std::sin(eps) * eq.y + std::cos(eps) * eq.z);
                pole = ob.orbitFrame * ecl;
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

    // ---- sky bake from the viewpoint, one face per frame while stale
    double dGal = glm::length(camGal - content_.host.center);
    for (const universe::GalaxyRecord& g : content_.galaxies) dGal = std::min(dGal, glm::length(camGal - g.model.center));
    // A full cycle (six faces, one per frame) always completes with the viewpoint and galaxy lists
    // captured at its start; if the camera moved meanwhile, another cycle follows.
    const double thr = std::max(0.5 * units::kParsec, 0.004 * dGal);
    if (glm::length(camGal - bakeCam_) > thr || remap) bakeDirty_ = true;
    if (bakeFace_ >= 6 && bakeDirty_) bakeFace_ = 0;
    if (bakeFace_ == 0) {
        bakeCam_ = camGal;
        buildGalaxyLists(camGal);
        bakeDirty_ = false;
    }
    if (bakeFace_ < 6) bakeFace(bakeFace_++, camGal);
}

}  // namespace bhs
