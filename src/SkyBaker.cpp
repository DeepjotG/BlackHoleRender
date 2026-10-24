#include "SkyBaker.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "Log.h"
#include "Settings.h"

namespace bhs {

namespace {

// Matches the struct in shaders/sky_bake.comp (std430, 4 x vec4).
struct GalaxyGpu {
    glm::vec4 dirCosR;   // xyz direction, w = cos(angular radius)
    glm::vec4 frame;     // xyz major-axis tangent, w = axis ratio (cos inclination)
    glm::vec4 color;     // rgb tint, a = radiance scale
    glm::vec4 params;    // x type (0 elliptical, 1 spiral, 2 barred), y arms, z pitch, w seed
    float angRadius;     // CPU only
    float pad[3];
};

// Direction of a cubemap texel centre (OpenGL face conventions).
glm::vec3 cubeDir(int face, float u, float v) {
    switch (face) {
        case 0: return glm::normalize(glm::vec3(1.0f, -v, -u));
        case 1: return glm::normalize(glm::vec3(-1.0f, -v, u));
        case 2: return glm::normalize(glm::vec3(u, 1.0f, v));
        case 3: return glm::normalize(glm::vec3(u, -1.0f, -v));
        case 4: return glm::normalize(glm::vec3(u, -v, 1.0f));
        default: return glm::normalize(glm::vec3(-u, -v, -1.0f));
    }
}

glm::vec3 randomUnit(std::mt19937& rng) {
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    for (;;) {
        glm::vec3 v(U(rng), U(rng), U(rng));
        const float l2 = glm::dot(v, v);
        if (l2 > 1e-4f && l2 <= 1.0f) return v / std::sqrt(l2);
    }
}

glm::vec3 perpendicular(const glm::vec3& n, std::mt19937& rng) {
    glm::vec3 r = randomUnit(rng);
    glm::vec3 p = r - n * glm::dot(r, n);
    if (glm::dot(p, p) < 1e-6f) p = glm::cross(n, glm::vec3(0, 1, 0));
    return glm::normalize(p);
}

}  // namespace

bool SkyBaker::init() {
    createCubemap(1);
    if (!prog_.loadCompute("sky_bake.comp")) {
        logError("sky bake shader failed to compile; the sky will be empty");
        return false;
    }
    glGenBuffers(1, &galaxyBuffer_);
    glGenBuffers(1, &tileBuffer_);
    glGenBuffers(1, &indexBuffer_);
    return true;
}

void SkyBaker::shutdown() {
    prog_.destroy();
    if (cube_) glDeleteTextures(1, &cube_);
    if (galaxyBuffer_) glDeleteBuffers(1, &galaxyBuffer_);
    if (tileBuffer_) glDeleteBuffers(1, &tileBuffer_);
    if (indexBuffer_) glDeleteBuffers(1, &indexBuffer_);
    cube_ = galaxyBuffer_ = tileBuffer_ = indexBuffer_ = 0;
}

void SkyBaker::createCubemap(int res) {
    if (cube_) glDeleteTextures(1, &cube_);
    glGenTextures(1, &cube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
    int levels = 1;
    for (int r = res; r > 1; r >>= 1) ++levels;
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, GL_R11F_G11F_B10F, res, res);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    std::vector<float> zeros(static_cast<size_t>(res) * res * 3, 0.0f);
    for (int f = 0; f < 6; ++f)
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, res, res, GL_RGB, GL_FLOAT, zeros.data());
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    res_ = res;
}

bool SkyBaker::needsRebake(const Settings& s) const {
    return s.skySeed != bakedSeed_ || s.skyResolution != bakedRes_ || s.galaxyCount != bakedGalaxies_ ||
           s.milkyWayBrightness != bakedMw_ || s.nebulaBrightness != bakedNeb_ || s.galaxyBrightness != bakedGal_;
}

void SkyBaker::buildGalaxies(const Settings& s) {
    std::mt19937 rng(s.skySeed * 2654435761u + 17u);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int n = std::max(0, s.galaxyCount);

    // Cluster centres: a fraction of galaxies gather around them.
    std::vector<glm::vec3> clusters;
    std::vector<float> clusterSpread;
    const int nClusters = 14 + static_cast<int>(U(rng) * 10.0f);
    for (int i = 0; i < nClusters; ++i) {
        clusters.push_back(randomUnit(rng));
        clusterSpread.push_back(glm::radians(2.0f + 6.0f * U(rng)));
    }

    std::vector<GalaxyGpu> gal;
    gal.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        GalaxyGpu g{};
        glm::vec3 dir;
        if (U(rng) < 0.45f && !clusters.empty()) {
            const int c = static_cast<int>(U(rng) * static_cast<float>(clusters.size())) % static_cast<int>(clusters.size());
            std::normal_distribution<float> N(0.0f, clusterSpread[static_cast<size_t>(c)]);
            glm::vec3 t1 = perpendicular(clusters[static_cast<size_t>(c)], rng);
            glm::vec3 t2 = glm::cross(clusters[static_cast<size_t>(c)], t1);
            dir = glm::normalize(clusters[static_cast<size_t>(c)] + t1 * N(rng) + t2 * N(rng));
        } else {
            dir = randomUnit(rng);
        }
        // Angular radius: mostly tiny smudges with a power-law tail of large, nearby galaxies.
        const float u = std::max(U(rng), 1e-4f);
        float radiusDeg = 0.045f * std::pow(u, -0.75f);            // 0.045 deg .. ~45 deg
        radiusDeg = std::min(radiusDeg, 7.0f);
        const float R = glm::radians(radiusDeg);
        const float type = U(rng) < 0.28f ? 0.0f : (U(rng) < 0.72f ? 1.0f : 2.0f);
        const float incl = glm::radians(U(rng) * 82.0f);
        const float q = std::max(std::cos(incl), 0.16f);
        glm::vec3 major = perpendicular(dir, rng);
        g.dirCosR = glm::vec4(dir, std::cos(R));
        g.frame = glm::vec4(major, type < 0.5f ? std::max(q, 0.45f) : q);
        glm::vec3 tint;
        if (type < 0.5f) tint = glm::vec3(1.0f, 0.86f, 0.70f);       // old yellow stars
        else tint = glm::mix(glm::vec3(0.92f, 0.90f, 0.86f), glm::vec3(0.78f, 0.84f, 1.0f), U(rng));
        const float radiance = 0.9f * (0.55f + 0.9f * U(rng)) * s.galaxyBrightness;
        g.color = glm::vec4(tint, radiance);
        const float arms = U(rng) < 0.72f ? 2.0f : (U(rng) < 0.5f ? 3.0f : 4.0f);
        const float pitch = 0.18f + 0.30f * U(rng);
        g.params = glm::vec4(type, arms, pitch, U(rng) * 100.0f);
        g.angRadius = R;
        gal.push_back(g);
    }

    // Bin galaxies into cube-face tiles by cone overlap.
    const int T = kTileGrid;
    const int tiles = 6 * T * T;
    std::vector<glm::vec3> tileCenter(static_cast<size_t>(tiles));
    std::vector<float> tileRadius(static_cast<size_t>(tiles));
    for (int f = 0; f < 6; ++f) {
        for (int j = 0; j < T; ++j) {
            for (int i = 0; i < T; ++i) {
                const float u0 = 2.0f * static_cast<float>(i) / T - 1.0f, u1 = 2.0f * static_cast<float>(i + 1) / T - 1.0f;
                const float v0 = 2.0f * static_cast<float>(j) / T - 1.0f, v1 = 2.0f * static_cast<float>(j + 1) / T - 1.0f;
                const glm::vec3 c = cubeDir(f, 0.5f * (u0 + u1), 0.5f * (v0 + v1));
                float maxAng = 0.0f;
                for (const glm::vec3& corner : {cubeDir(f, u0, v0), cubeDir(f, u1, v0), cubeDir(f, u0, v1), cubeDir(f, u1, v1)})
                    maxAng = std::max(maxAng, std::acos(std::clamp(glm::dot(c, corner), -1.0f, 1.0f)));
                const size_t idx = static_cast<size_t>((f * T + j) * T + i);
                tileCenter[idx] = c;
                tileRadius[idx] = maxAng;
            }
        }
    }
    std::vector<std::vector<uint32_t>> lists(static_cast<size_t>(tiles));
    for (size_t gi = 0; gi < gal.size(); ++gi) {
        const glm::vec3 d(gal[gi].dirCosR);
        for (int t = 0; t < tiles; ++t) {
            const float ang = std::acos(std::clamp(glm::dot(d, tileCenter[static_cast<size_t>(t)]), -1.0f, 1.0f));
            if (ang < gal[gi].angRadius + tileRadius[static_cast<size_t>(t)]) lists[static_cast<size_t>(t)].push_back(static_cast<uint32_t>(gi));
        }
    }
    std::vector<uint32_t> offsets(static_cast<size_t>(tiles) * 2);
    std::vector<uint32_t> indices;
    size_t maxPerTile = 0;
    for (int t = 0; t < tiles; ++t) {
        offsets[static_cast<size_t>(t) * 2 + 0] = static_cast<uint32_t>(indices.size());
        offsets[static_cast<size_t>(t) * 2 + 1] = static_cast<uint32_t>(lists[static_cast<size_t>(t)].size());
        maxPerTile = std::max(maxPerTile, lists[static_cast<size_t>(t)].size());
        indices.insert(indices.end(), lists[static_cast<size_t>(t)].begin(), lists[static_cast<size_t>(t)].end());
    }
    if (indices.empty()) indices.push_back(0);
    if (gal.empty()) gal.push_back(GalaxyGpu{});

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, galaxyBuffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(gal.size() * sizeof(GalaxyGpu)), gal.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tileBuffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(offsets.size() * sizeof(uint32_t)), offsets.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, indexBuffer_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    logInfo("sky: {} galaxies binned into {} tiles ({} index entries, max {} per tile)", n, tiles, indices.size(), maxPerTile);
}

void SkyBaker::bake(const Settings& s) {
    const double t0 = glfwGetTime();
    if (res_ != s.skyResolution) createCubemap(s.skyResolution);
    bakedSeed_ = s.skySeed;
    bakedRes_ = s.skyResolution;
    bakedGalaxies_ = s.galaxyCount;
    bakedMw_ = s.milkyWayBrightness;
    bakedNeb_ = s.nebulaBrightness;
    bakedGal_ = s.galaxyBrightness;
    if (!prog_.valid()) return;

    buildGalaxies(s);

    // Galactic frame for the Milky Way band, derived from the seed.
    std::mt19937 rng(s.skySeed * 747796405u + 5u);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    glm::vec3 north = glm::normalize(glm::vec3(0.35f + 0.3f * U(rng), 0.55f + 0.3f * U(rng), -0.6f + 0.4f * U(rng)));
    glm::vec3 centre = glm::normalize(glm::cross(north, randomUnit(rng)));

    prog_.use();
    prog_.set("uRes", res_);
    prog_.set("uSeed", s.skySeed);
    prog_.set("uTileGrid", kTileGrid);
    prog_.set("uMilkyWay", s.milkyWayBrightness);
    prog_.set("uNebula", s.nebulaBrightness);
    prog_.set("uGalacticNorth", north);
    prog_.set("uGalacticCentre", centre);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, galaxyBuffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, tileBuffer_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, indexBuffer_);
    glBindImageTexture(0, cube_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R11F_G11F_B10F);

    // Dispatch in bands so a single job never runs long enough to trip the GPU watchdog.
    const int band = 256;
    for (int face = 0; face < 6; ++face) {
        prog_.set("uFace", face);
        for (int row = 0; row < res_; row += band) {
            prog_.set("uRow0", row);
            const int rows = std::min(band, res_ - row);
            glDispatchCompute(static_cast<GLuint>((res_ + 15) / 16), static_cast<GLuint>((rows + 15) / 16), 1);
            glFlush();
        }
        glFinish();
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube_);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glFinish();
    lastBakeMs_ = (glfwGetTime() - t0) * 1000.0;
    logInfo("sky baked: {}^2 x 6, seed {}, {} galaxies in {:.0f} ms", res_, s.skySeed, s.galaxyCount, lastBakeMs_);
}

}  // namespace bhs
