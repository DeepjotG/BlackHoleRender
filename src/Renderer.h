#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <string>

#include "Shader.h"

namespace bhs {

class Camera;
struct Settings;

struct FrameInput {
    const Camera* camera = nullptr;
    const Settings* settings = nullptr;
    float time = 0.0f;   // disk animation time (seconds)
    float dt = 0.0f;
    int windowW = 0, windowH = 0;
    GLuint skyCubemap = 0, noise3d = 0, blackbodyLut = 0;
    bool resetAccumulation = false;
    int debugView = 0;
};

// Owns the GL pass chain:  trace (HDR, internal res) -> accumulate -> upscale -> bloom -> composite.
class Renderer {
public:
    bool init();
    void shutdown();
    void render(const FrameInput& in);

    bool checkShaderReload();   // polls file mtimes; returns true if anything was rebuilt
    bool reloadShaders();
    void resetAccumulation() { accumCount_ = 0; }
    int accumulatedFrames() const { return accumCount_; }
    float gpuMs() const { return gpuMs_; }
    glm::ivec2 internalSize() const { return {trace_.w, trace_.h}; }
    float dynamicScale() const { return dynScale_; }
    const std::string& shaderLog() const { return shaderLog_; }
    bool shadersOk() const { return traceProg_.valid() && compositeProg_.valid(); }

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
    void setTraceUniforms(const FrameInput& in, const glm::vec2& jitter);

    ShaderProgram traceProg_, compositeProg_;
    Target trace_;
    GLuint vao_ = 0;

    static constexpr int kQueryRing = 4;
    GLuint queries_[kQueryRing] = {};
    int queryWrite_ = 0;
    int queriesIssued_ = 0;
    float gpuMs_ = 0.0f;

    int accumCount_ = 0;
    float dynScale_ = 1.0f;
    float dynTimer_ = 0.0f;
    std::string shaderLog_;
};

}  // namespace bhs
