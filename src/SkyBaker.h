#pragma once
#include <glad/gl.h>

#include <cstdint>

#include "Shader.h"

namespace bhs {

struct Settings;

// Bakes the procedural deep-space backdrop (Milky Way, nebulae, thousands of galaxies)
// into an HDR cubemap with a compute shader. Stars are rendered analytically at
// runtime instead (see shaders/sky.glsl) so they stay sharp under lensing.
class SkyBaker {
public:
    static constexpr int kTileGrid = 16;   // tiles per cube-face edge for galaxy binning

    bool init();
    void shutdown();
    void bake(const Settings& s);
    bool needsRebake(const Settings& s) const;
    GLuint cubemap() const { return cube_; }
    double lastBakeMs() const { return lastBakeMs_; }
    int resolution() const { return res_; }
    int galaxyCount() const { return bakedGalaxies_; }

private:
    void createCubemap(int res);
    void buildGalaxies(const Settings& s);

    GLuint cube_ = 0;
    GLuint galaxyBuffer_ = 0, tileBuffer_ = 0, indexBuffer_ = 0;
    ShaderProgram prog_;
    int res_ = 0;
    uint32_t bakedSeed_ = 0;
    int bakedRes_ = 0;
    int bakedGalaxies_ = -1;
    float bakedMw_ = -1.0f, bakedNeb_ = -1.0f, bakedGal_ = -1.0f;
    double lastBakeMs_ = 0.0;
};

}  // namespace bhs
