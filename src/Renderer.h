#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <string>
#include <vector>

#include "Shader.h"

namespace bhs {

class Camera;
struct Settings;

// One gas volume of the star-disruption simulation (see tde/TdeSim.h).
struct GasVolumeRef {
    GLuint texture = 0;
    glm::vec3 center{0.0f};
    float halfSize = 0.0f;
    int resolution = 0;
};

struct FrameInput {
    const Camera* camera = nullptr;
    const Settings* settings = nullptr;
    float time = 0.0f;   // seconds since start (film grain)
    float diskTime = 0.0f;   // disk flow clock in M, wrapped to the noise cross-fade period
    float dt = 0.0f;
    int windowW = 0, windowH = 0;
    GLuint skyCubemap = 0, noise3d = 0, blackbodyLut = 0;
    bool resetAccumulation = false;
    bool diskAnimating = true;
    GLuint targetFbo = 0;   // final framebuffer (0 = window)
    float extraGpuMs = 0.0f; // GPU work outside the renderer (bridge readback), for frame-time targets
    const std::vector<struct BodyGpu>* bodies = nullptr;
    bool farMode = false;    // camera far from the hole: straight rays
    const GasVolumeRef* gas = nullptr;
    int gasCount = 0;
    // 3D universe sky (see UniverseScene)
    bool universe = false;
    GLuint uniCube = 0, uniStars = 0, uniTiles = 0;
    float uniStarScale = 1.0f;
    float uniSkyScale = 1.0f;     // exposure of the universe's diffuse light
    int uniTileGrid = 128;
    int debugView = 0;
};

// Owns the GL pass chain:
//   trace (HDR, internal res) -> accumulate -> upscale + sharpen -> bloom -> composite (tonemap)
class Renderer {
public:
    bool init();
    void shutdown();
    void render(const FrameInput& in);

    // Offline high-resolution capture with `samples` accumulated jittered frames; writes a PNG.
    bool captureHighRes(const FrameInput& in, int width, int height, int samples, const std::string& path);

    bool checkShaderReload();
    bool reloadShaders();
    void resetAccumulation() { accumCount_ = 0; }
    int accumulatedFrames() const { return accumCount_; }
    float gpuMs() const { return gpuMs_; }
    glm::ivec2 internalSize() const { return {trace_.w, trace_.h}; }
    float dynamicScale() const { return dynScale_; }
    float autoExposureGain() const { return autoGain_; }
    const std::string& shaderLog() const { return shaderLog_; }
    bool shadersOk() const;

private:
    struct Target {
        GLuint fbo = 0, tex = 0;
        int w = 0, h = 0;
        GLenum fmt = GL_RGBA16F;
    };
    void ensureTarget(Target& t, int w, int h, GLenum fmt);
    void destroyTarget(Target& t);
    void drawFullscreen();
    void beginTimer();
    void endTimer();
    void updateDynamicScale(const FrameInput& in);
    void setTraceUniforms(const FrameInput& in, const Target& dst, const glm::vec2& jitter);

    void tracePass(const FrameInput& in, Target& dst, const glm::vec2& jitter, bool banded);
    const Target& accumulatePass(const FrameInput& in, const Target& current, int sampleIndex, int cap);
    void upscalePass(const FrameInput& in, const Target& src, Target& dst);
    void bloomPass(const FrameInput& in, const Target& src);
    void compositePass(const FrameInput& in, const Target& hdr, GLuint dstFbo, int w, int h);
    void updateAutoExposure(const FrameInput& in, const Target& hdr);

    // Tracer variants: with/without body code and with/without gas code.
    ShaderProgram traceProg_, traceBodiesProg_, traceGasProg_, traceGasBodiesProg_;
    ShaderProgram accumProg_, upscaleProg_, bloomDownProg_, bloomUpProg_, compositeProg_;
    ShaderProgram& traceFor(const FrameInput& in);
    Target trace_, accum_[2], upscaled_, lumaProbe_, captureOut_;
    static constexpr int kMaxBloomLevels = 8;
    Target bloom_[kMaxBloomLevels];
    int bloomLevels_ = 0;
    int accumPing_ = 0;
    GLuint vao_ = 0;
    GLuint bodyBuffer_ = 0;
    size_t bodyCapacity_ = 0;

    static constexpr int kQueryRing = 4;
    GLuint queries_[kQueryRing] = {};
    int queryWrite_ = 0;
    int queriesIssued_ = 0;
    float gpuMs_ = 0.0f;

    int accumCount_ = 0;
    float dynScale_ = 1.0f;
    float dynTimer_ = 0.0f;
    float autoGain_ = 1.0f;
    float autoTimer_ = 0.0f;
    std::string shaderLog_;
};

}  // namespace bhs
