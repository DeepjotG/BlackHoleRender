#pragma once
#include <glad/gl.h>

#include "Shader.h"

namespace bhs {

// GPU resources used by the accretion disk and star shading:
//   * a blackbody lookup table (temperature -> linear sRGB colour and luminance)
//   * a tileable 3D fbm noise texture for the volumetric disk
class DiskTextures {
public:
    static constexpr float kTempMin = 1000.0f;
    static constexpr float kTempMax = 40000.0f;
    static constexpr int kLutSize = 256;
    static constexpr int kNoiseSize = 128;

    bool init();
    void shutdown();
    GLuint blackbodyLut() const { return lut_; }
    GLuint noise3d() const { return noise_; }

private:
    void bakeBlackbodyLut();
    bool bakeNoise();
    GLuint lut_ = 0;
    GLuint noise_ = 0;
    ShaderProgram noiseProg_;
};

}  // namespace bhs
