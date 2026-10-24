#include "Renderer.h"

#include <algorithm>
#include <cmath>

#include "Camera.h"
#include "Log.h"
#include "Settings.h"

namespace bhs {

namespace {
float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    int i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}
}  // namespace

bool Renderer::init() {
    glGenVertexArrays(1, &vao_);
    glGenQueries(kQueryRing, queries_);
    return reloadShaders();
}

void Renderer::shutdown() {
    traceProg_.destroy();
    compositeProg_.destroy();
    destroyTarget(trace_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    glDeleteQueries(kQueryRing, queries_);
    vao_ = 0;
}

bool Renderer::reloadShaders() {
    bool ok = true;
    shaderLog_.clear();
    if (!traceProg_.loadGraphics("fullscreen.vert", "blackhole.frag")) { ok = false; shaderLog_ += traceProg_.log(); }
    if (!compositeProg_.loadGraphics("fullscreen.vert", "composite.frag")) { ok = false; shaderLog_ += compositeProg_.log(); }
    if (ok) logInfo("shaders compiled");
    accumCount_ = 0;
    return ok;
}

bool Renderer::checkShaderReload() {
    bool any = false;
    if (traceProg_.reloadIfChanged()) { any = true; shaderLog_ = traceProg_.log(); }
    if (compositeProg_.reloadIfChanged()) { any = true; shaderLog_ += compositeProg_.log(); }
    if (any) {
        logInfo("shaders hot-reloaded{}", shaderLog_.empty() ? "" : " (with errors)");
        accumCount_ = 0;
    }
    return any;
}

void Renderer::ensureTarget(Target& t, int w, int h, GLenum fmt) {
    if (t.fbo && t.w == w && t.h == h && t.fmt == fmt) return;
    destroyTarget(t);
    t.w = w; t.h = h; t.fmt = fmt;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logError("framebuffer incomplete ({}x{})", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    accumCount_ = 0;
}

void Renderer::destroyTarget(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t.fbo = t.tex = 0;
    t.w = t.h = 0;
}

void Renderer::drawFullscreen() {
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::beginTimer() {
    // Collect the oldest finished query before issuing a new one.
    if (queriesIssued_ >= kQueryRing) {
        const int oldest = queryWrite_;  // ring slot about to be reused
        GLuint available = 0;
        glGetQueryObjectuiv(queries_[oldest], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(queries_[oldest], GL_QUERY_RESULT, &ns);
            const float ms = static_cast<float>(ns) * 1e-6f;
            gpuMs_ = gpuMs_ <= 0.0f ? ms : gpuMs_ * 0.8f + ms * 0.2f;
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, queries_[queryWrite_]);
}

void Renderer::endTimer() {
    glEndQuery(GL_TIME_ELAPSED);
    queryWrite_ = (queryWrite_ + 1) % kQueryRing;
    ++queriesIssued_;
}

void Renderer::updateDynamicScale(const FrameInput& in) {
    const Settings& s = *in.settings;
    if (!s.dynamicResolution) { dynScale_ = 1.0f; return; }
    dynTimer_ += in.dt;
    if (dynTimer_ < 0.4f || gpuMs_ <= 0.0f) return;
    dynTimer_ = 0.0f;
    const float ratio = s.targetFrameMs / gpuMs_;
    float target = dynScale_ * std::sqrt(std::clamp(ratio, 0.5f, 2.0f));
    target = std::clamp(target, 0.3f, 1.0f);
    if (std::abs(target - dynScale_) > 0.03f) {
        dynScale_ = target;
        accumCount_ = 0;
    }
}

void Renderer::setTraceUniforms(const FrameInput& in, const glm::vec2& jitter) {
    const Settings& s = *in.settings;
    const Camera& cam = *in.camera;
    ShaderProgram& p = traceProg_;
    const float tanHalf = std::tan(glm::radians(s.fovDeg) * 0.5f);
    const float pixelAngle = 2.0f * tanHalf / static_cast<float>(trace_.h);
    p.set("uResolution", glm::vec2(trace_.w, trace_.h));
    p.set("uJitter", jitter);
    p.set("uCamPos", cam.position());
    p.set("uCamBasis", cam.basis());
    p.set("uTanHalfFov", tanHalf);
    p.set("uTime", in.time);
    p.set("uLensing", s.lensing ? 1 : 0);
    p.set("uMaxSteps", s.maxSteps);
    p.set("uStepScale", s.stepScale);
    p.set("uDiskStep", s.diskStep);
    p.set("uDiskMode", s.diskMode);
    p.set("uDopplerStrength", s.dopplerStrength);
    p.set("uShadowOverlay", s.showShadowOverlay ? 1 : 0);
    p.set("uPixelOmega0", pixelAngle * pixelAngle);
    p.set("uPixelAngle", pixelAngle);
    p.set("uDebugView", in.debugView);
    // disk
    p.set("uDiskInner", s.diskInner);
    p.set("uDiskOuter", s.diskOuter);
    p.set("uDiskThickness", s.diskThickness);
    p.set("uDiskDensity", s.diskDensity);
    p.set("uDiskOpacity", s.diskOpacity);
    p.set("uDiskPeakTemp", s.diskPeakTemp);
    p.set("uDiskBrightness", s.diskBrightness);
    p.set("uDiskNoiseScale", s.diskNoiseScale);
    p.set("uDiskNoiseStrength", s.diskNoiseStrength);
    p.set("uDiskSpeed", s.diskSpeed);
    // sky
    p.set("uSkyBrightness", s.skyBrightness);
    p.set("uStarDensity", s.starDensity);
    p.set("uStarBrightness", s.starBrightness);
    // samplers
    p.set("uSkyCube", 0);
    p.set("uNoise3d", 1);
    p.set("uBlackbodyLut", 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, in.skyCubemap);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, in.noise3d);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_1D, in.blackbodyLut);
}

void Renderer::render(const FrameInput& in) {
    const Settings& s = *in.settings;
    updateDynamicScale(in);
    const float scale = std::clamp(s.renderScale * dynScale_, 0.1f, 2.0f);
    const int iw = std::max(16, static_cast<int>(std::lround(in.windowW * scale)));
    const int ih = std::max(16, static_cast<int>(std::lround(in.windowH * scale)));
    ensureTarget(trace_, iw, ih, GL_RGBA16F);
    if (in.resetAccumulation) accumCount_ = 0;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    beginTimer();

    // ---- trace pass ----
    glBindFramebuffer(GL_FRAMEBUFFER, trace_.fbo);
    glViewport(0, 0, iw, ih);
    traceProg_.use();
    setTraceUniforms(in, glm::vec2(0.0f));
    drawFullscreen();

    // ---- composite ----
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, in.windowW, in.windowH);
    compositeProg_.use();
    compositeProg_.set("uHdr", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, trace_.tex);
    compositeProg_.set("uResolution", glm::vec2(in.windowW, in.windowH));
    compositeProg_.set("uExposure", s.exposure);
    compositeProg_.set("uTonemap", s.tonemap);
    compositeProg_.set("uVignette", s.vignette);
    compositeProg_.set("uGrain", s.grain);
    compositeProg_.set("uSaturation", s.saturation);
    compositeProg_.set("uTime", in.time);
    drawFullscreen();

    endTimer();
    ++accumCount_;
    (void)halton;
}

}  // namespace bhs
