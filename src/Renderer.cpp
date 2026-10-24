#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Camera.h"
#include "Capture.h"
#include "Log.h"
#include "Settings.h"
#include "Bodies.h"

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
glm::vec2 jitterFor(int sample) {
    if (sample <= 0) return glm::vec2(0.0f);
    return glm::vec2(halton(sample, 2) - 0.5f, halton(sample, 3) - 0.5f);
}
float halfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
    return s ? -v : v;
}
}  // namespace

bool Renderer::init() {
    glGenVertexArrays(1, &vao_);
    glGenQueries(kQueryRing, queries_);
    return reloadShaders();
}

void Renderer::shutdown() {
    for (ShaderProgram* p : {&traceProg_, &traceBodiesProg_, &traceGasProg_, &traceGasBodiesProg_, &accumProg_, &upscaleProg_, &bloomDownProg_, &bloomUpProg_, &compositeProg_, &gasBlurProg_, &gasPeakProg_}) p->destroy();
    if (gasPeakBuf_[0]) glDeleteBuffers(2, gasPeakBuf_);
    for (auto& pair : gasSmooth_)
        if (pair[0]) glDeleteTextures(2, pair);
    destroyTarget(trace_);
    destroyTarget(accum_[0]);
    destroyTarget(accum_[1]);
    destroyTarget(upscaled_);
    destroyTarget(lumaProbe_);
    destroyTarget(captureOut_);
    for (Target& b : bloom_) destroyTarget(b);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (bodyBuffer_) glDeleteBuffers(1, &bodyBuffer_);
    bodyBuffer_ = 0;
    bodyCapacity_ = 0;
    glDeleteQueries(kQueryRing, queries_);
    vao_ = 0;
}

bool Renderer::shadersOk() const {
    return traceProg_.valid() && traceBodiesProg_.valid() && traceGasProg_.valid() && traceGasBodiesProg_.valid() &&
           accumProg_.valid() && upscaleProg_.valid() && bloomDownProg_.valid() &&
           bloomUpProg_.valid() && compositeProg_.valid();
}

bool Renderer::reloadShaders() {
    bool ok = true;
    shaderLog_.clear();
    struct Entry { ShaderProgram* p; const char* frag; };
    traceProg_.setDefines({"BHS_BODIES 0", "BHS_GAS 0"});
    traceBodiesProg_.setDefines({"BHS_BODIES 1", "BHS_GAS 0"});
    traceGasProg_.setDefines({"BHS_BODIES 0", "BHS_GAS 1"});
    traceGasBodiesProg_.setDefines({"BHS_BODIES 1", "BHS_GAS 1"});
    const Entry entries[] = {
        {&traceProg_, "blackhole.frag"},   {&traceBodiesProg_, "blackhole.frag"},
        {&traceGasProg_, "blackhole.frag"}, {&traceGasBodiesProg_, "blackhole.frag"},
        {&accumProg_, "accumulate.frag"}, {&upscaleProg_, "upscale.frag"},
        {&bloomDownProg_, "bloom_down.frag"}, {&bloomUpProg_, "bloom_up.frag"}, {&compositeProg_, "composite.frag"},
    };
    for (const Entry& e : entries) {
        if (!e.p->loadGraphics("fullscreen.vert", e.frag)) {
            ok = false;
            shaderLog_ += e.p->log();
        }
    }
    if (!gasBlurProg_.loadCompute("gas_blur.comp")) {
        ok = false;
        shaderLog_ += gasBlurProg_.log();
    }
    if (!gasPeakProg_.loadCompute("gas_peak.comp")) {
        ok = false;
        shaderLog_ += gasPeakProg_.log();
    }
    if (ok) logInfo("shaders compiled");
    accumCount_ = 0;
    return ok;
}

bool Renderer::checkShaderReload() {
    bool any = false;
    for (ShaderProgram* p : {&traceProg_, &traceBodiesProg_, &traceGasProg_, &traceGasBodiesProg_, &accumProg_, &upscaleProg_,
                             &bloomDownProg_, &bloomUpProg_, &compositeProg_, &gasBlurProg_, &gasPeakProg_}) {
        if (p->reloadIfChanged()) {
            any = true;
            shaderLog_ = p->log();
        }
    }
    if (any) {
        logInfo("shaders hot-reloaded{}", shaderLog_.empty() ? "" : " (with errors)");
        accumCount_ = 0;
    }
    return any;
}

void Renderer::ensureTarget(Target& t, int w, int h, GLenum fmt) {
    if (t.fbo && t.w == w && t.h == h && t.fmt == fmt) return;
    destroyTarget(t);
    t.w = w;
    t.h = h;
    t.fmt = fmt;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glClearTexImage(t.tex, 0, GL_RGBA, GL_FLOAT, nullptr);   // never sample undefined history
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
    if (queriesIssued_ >= kQueryRing) {
        const int oldest = queryWrite_;
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
    if (!s.dynamicResolution) {
        dynScale_ = 1.0f;
        return;
    }
    dynTimer_ += in.dt;
    if (dynTimer_ < 0.4f || gpuMs_ <= 0.0f) return;
    dynTimer_ = 0.0f;
    const float ratio = s.targetFrameMs / (gpuMs_ + in.extraGpuMs);
    float target = dynScale_ * std::sqrt(std::clamp(ratio, 0.5f, 2.0f));
    target = std::clamp(target, 0.3f, 1.0f);
    if (std::abs(target - dynScale_) > 0.03f) {
        dynScale_ = target;
        accumCount_ = 0;
    }
}

void Renderer::setTraceUniforms(const FrameInput& in, const Target& dst, const glm::vec2& jitter) {
    const Settings& s = *in.settings;
    const Camera& cam = *in.camera;
    ShaderProgram& p = traceFor(in);
    const float tanHalf = std::tan(glm::radians(s.fovDeg) * 0.5f);
    const float pixelAngle = 2.0f * tanHalf / static_cast<float>(dst.h);
    p.set("uResolution", glm::vec2(dst.w, dst.h));
    p.set("uJitter", jitter);
    p.set("uCamPos", glm::vec3(cam.position()));
    p.set("uCamBasis", cam.basis());
    p.set("uTanHalfFov", tanHalf);
    p.set("uTime", in.time);
    p.set("uDiskTime", in.diskTime);
    p.set("uLensing", s.lensing ? 1 : 0);
    p.set("uFarMode", in.farMode ? 1 : 0);
    p.set("uUniverse", in.universe ? 1 : 0);
    p.set("uUniCube", 3);
    p.set("uUniStarScale", in.uniStarScale);
    p.set("uUniSkyScale", in.uniSkyScale);
    p.set("uUniT", in.uniTileGrid);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_CUBE_MAP, in.universe ? in.uniCube : in.skyCubemap);
    if (in.universe) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, in.uniStars);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, in.uniTiles);
    }
    glActiveTexture(GL_TEXTURE0);
    // Bodies (planets, stars): shader storage buffer at binding 3.
    const size_t nb = in.bodies ? in.bodies->size() : 0;
    if (!bodyBuffer_) glGenBuffers(1, &bodyBuffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bodyBuffer_);
    const size_t need = std::max<size_t>(nb, 1) * sizeof(BodyGpu);
    if (need > bodyCapacity_) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(need * 2), nullptr, GL_DYNAMIC_DRAW);
        bodyCapacity_ = need * 2;
    }
    if (nb) glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(nb * sizeof(BodyGpu)), in.bodies->data());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, bodyBuffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    p.set("uBodyCount", static_cast<int>(nb));
    // Star-disruption gas volumes on texture units 4..7.
    const int ng = (in.gas && in.gasCount > 0) ? std::min(in.gasCount, 4) : 0;
    p.set("uGasCount", ng);
    // BHS_GAS_BRIGHTNESS overrides the setting (tuning captures).
    static const float gbOverride = std::getenv("BHS_GAS_BRIGHTNESS") ? static_cast<float>(std::atof(std::getenv("BHS_GAS_BRIGHTNESS"))) : -1.0f;
    p.set("uGasBrightness", gbOverride > 0.0f ? gbOverride : s.gasBrightness);
    for (int i = 0; i < 4; ++i) p.set("uGasLive[" + std::to_string(i) + "]", i < ng ? gasLive_[i] : 0);
    p.set("uGasOpacity", s.gasOpacity);
    for (int i = 0; i < 4; ++i) {
        const std::string idx = "[" + std::to_string(i) + "]";
        p.set("uGasTex" + idx, 4 + i);
        glActiveTexture(GL_TEXTURE4 + i);
        if (i < ng) {
            glBindTexture(GL_TEXTURE_3D, gasSmooth_[i][0] ? gasSmooth_[i][0] : in.gas[i].texture);
            p.set("uGasBox" + idx, glm::vec4(in.gas[i].center, in.gas[i].halfSize));
            p.set("uGasRes" + idx, static_cast<float>(in.gas[i].resolution));
        } else {
            glBindTexture(GL_TEXTURE_3D, in.noise3d);   // any valid 3D texture keeps samplers complete
        }
    }
    glActiveTexture(GL_TEXTURE0);
    p.set("uMaxSteps", s.maxSteps);
    p.set("uStepScale", s.stepScale);
    p.set("uDiskStep", s.diskStep);
    p.set("uDiskMode", s.diskMode);
    p.set("uDopplerStrength", s.dopplerStrength);
    p.set("uShadowOverlay", s.showShadowOverlay ? 1 : 0);
    p.set("uPixelOmega0", pixelAngle * pixelAngle);
    p.set("uPixelAngle", pixelAngle);
    p.set("uDebugView", in.debugView);
    p.set("uDiskInner", s.diskInner);
    p.set("uDiskOuter", s.diskOuter);
    p.set("uDiskThickness", s.diskThickness);
    p.set("uDiskDensity", s.diskDensity);
    p.set("uDiskOpacity", s.diskOpacity);
    p.set("uDiskPeakTemp", s.diskPeakTemp);
    p.set("uDiskBrightness", s.diskBrightness * in.diskFade);
    p.set("uDiskNoiseScale", s.diskNoiseScale);
    p.set("uDiskNoiseStrength", s.diskNoiseStrength);
    p.set("uDiskWarmth", s.diskWarmth);
    p.set("uSkyBrightness", s.skyBrightness);
    p.set("uStarDensity", s.starDensity);
    p.set("uStarBrightness", s.starBrightness);
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

ShaderProgram& Renderer::traceFor(const FrameInput& in) {
    const bool bodies = in.bodies && !in.bodies->empty();
    const bool gas = in.gas && in.gasCount > 0;
    if (gas) return bodies ? traceGasBodiesProg_ : traceGasProg_;
    return bodies ? traceBodiesProg_ : traceProg_;
}

void Renderer::tracePass(const FrameInput& in, Target& dst, const glm::vec2& jitter, bool banded) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    traceFor(in).use();
    setTraceUniforms(in, dst, jitter);
    if (!banded) {
        drawFullscreen();
        return;
    }
    // Split tall renders into bands so no single GPU job runs long enough to trip the watchdog.
    glEnable(GL_SCISSOR_TEST);
    const int band = 128;
    for (int y = 0; y < dst.h; y += band) {
        glScissor(0, y, dst.w, std::min(band, dst.h - y));
        drawFullscreen();
        glFlush();
    }
    glDisable(GL_SCISSOR_TEST);
}

const Renderer::Target& Renderer::accumulatePass(const FrameInput& in, const Target& current, int sampleIndex, int cap) {
    Target& prev = accum_[accumPing_];
    Target& next = accum_[accumPing_ ^ 1];
    ensureTarget(prev, current.w, current.h, GL_RGBA32F);
    ensureTarget(next, current.w, current.h, GL_RGBA32F);
    const int n = std::min(sampleIndex, std::max(cap - 1, 0));
    const float weight = (sampleIndex == 0) ? 1.0f : 1.0f / static_cast<float>(n + 1);
    glBindFramebuffer(GL_FRAMEBUFFER, next.fbo);
    glViewport(0, 0, next.w, next.h);
    accumProg_.use();
    accumProg_.set("uCurrent", 0);
    accumProg_.set("uPrevious", 1);
    accumProg_.set("uWeight", weight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, prev.tex);
    drawFullscreen();
    accumPing_ ^= 1;
    (void)in;
    return next;
}

void Renderer::upscalePass(const FrameInput& in, const Target& src, Target& dst) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    upscaleProg_.use();
    upscaleProg_.set("uSrc", 0);
    upscaleProg_.set("uSrcTexel", glm::vec2(1.0f / src.w, 1.0f / src.h));
    upscaleProg_.set("uSharpen", in.settings->sharpen);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src.tex);
    drawFullscreen();
}

void Renderer::bloomPass(const FrameInput& in, const Target& src) {
    const Settings& s = *in.settings;
    // Build the chain of half-resolution targets.
    bloomLevels_ = 0;
    int w = src.w / 2, h = src.h / 2;
    while (bloomLevels_ < kMaxBloomLevels && w >= 8 && h >= 8) {
        ensureTarget(bloom_[bloomLevels_], w, h, GL_R11F_G11F_B10F);
        ++bloomLevels_;
        w /= 2;
        h /= 2;
    }
    if (bloomLevels_ == 0) return;
    glDisable(GL_BLEND);
    bloomDownProg_.use();
    bloomDownProg_.set("uSrc", 0);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < bloomLevels_; ++i) {
        const Target& from = (i == 0) ? src : bloom_[i - 1];
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_[i].fbo);
        glViewport(0, 0, bloom_[i].w, bloom_[i].h);
        bloomDownProg_.set("uSrcTexel", glm::vec2(1.0f / from.w, 1.0f / from.h));
        bloomDownProg_.set("uThreshold", i == 0 ? s.bloomThreshold : -1.0f);
        glBindTexture(GL_TEXTURE_2D, from.tex);
        drawFullscreen();
    }
    // Upsample additively back down the chain.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    bloomUpProg_.use();
    bloomUpProg_.set("uSrc", 0);
    bloomUpProg_.set("uScale", 1.0f);
    for (int i = bloomLevels_ - 2; i >= 0; --i) {
        const Target& from = bloom_[i + 1];
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_[i].fbo);
        glViewport(0, 0, bloom_[i].w, bloom_[i].h);
        bloomUpProg_.set("uSrcTexel", glm::vec2(1.0f / from.w, 1.0f / from.h));
        glBindTexture(GL_TEXTURE_2D, from.tex);
        drawFullscreen();
    }
    glDisable(GL_BLEND);
}

void Renderer::updateAutoExposure(const FrameInput& in, const Target& hdr) {
    const Settings& s = *in.settings;
    if (!s.autoExposure) {
        autoGain_ = 1.0f;
        return;
    }
    autoTimer_ += in.dt;
    if (autoTimer_ < 0.1f) return;
    autoTimer_ = 0.0f;
    ensureTarget(lumaProbe_, 32, 18, GL_RGBA16F);
    glBindFramebuffer(GL_FRAMEBUFFER, lumaProbe_.fbo);
    glViewport(0, 0, lumaProbe_.w, lumaProbe_.h);
    bloomDownProg_.use();
    bloomDownProg_.set("uSrc", 0);
    bloomDownProg_.set("uSrcTexel", glm::vec2(1.0f / hdr.w, 1.0f / hdr.h) * 8.0f);
    bloomDownProg_.set("uThreshold", -1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdr.tex);
    drawFullscreen();
    std::vector<uint16_t> px(static_cast<size_t>(lumaProbe_.w) * lumaProbe_.h * 4);
    glReadPixels(0, 0, lumaProbe_.w, lumaProbe_.h, GL_RGBA, GL_HALF_FLOAT, px.data());
    double sumLog = 0.0;
    int n = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        const float r = halfToFloat(px[i]), g = halfToFloat(px[i + 1]), b = halfToFloat(px[i + 2]);
        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        if (std::isfinite(lum)) {
            sumLog += std::log(static_cast<double>(lum) + 1e-4);
            ++n;
        }
    }
    if (n == 0) return;
    const float avg = static_cast<float>(std::exp(sumLog / n));
    const float target = std::clamp(0.25f / std::max(avg, 1e-4f), 0.15f, 12.0f);
    const float k = 1.0f - std::exp(-0.1f / 0.9f);
    autoGain_ += (target - autoGain_) * k;
}

void Renderer::compositePass(const FrameInput& in, const Target& hdr, GLuint dstFbo, int w, int h) {
    const Settings& s = *in.settings;
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, w, h);
    compositeProg_.use();
    compositeProg_.set("uHdr", 0);
    compositeProg_.set("uBloom", 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdr.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomLevels_ > 0 ? bloom_[0].tex : hdr.tex);
    compositeProg_.set("uResolution", glm::vec2(w, h));
    compositeProg_.set("uExposure", s.exposure * autoGain_);
    compositeProg_.set("uTonemap", s.tonemap);
    compositeProg_.set("uVignette", s.vignette);
    compositeProg_.set("uGrain", s.grain);
    compositeProg_.set("uSaturation", s.saturation);
    compositeProg_.set("uBloomIntensity", bloomLevels_ > 0 ? s.bloomIntensity : 0.0f);
    compositeProg_.set("uChroma", s.chromaticAberration);
    compositeProg_.set("uTime", in.time);
    drawFullscreen();
}

// Smooths each star-disruption gas volume: x, y and z passes of a 5-tap blur, ping-ponging between
// two textures per volume, ending in gasSmooth_[i][0].
void Renderer::smoothGas(const FrameInput& in) {
    const int ng = (in.gas && in.gasCount > 0) ? std::min(in.gasCount, 4) : 0;
    static bool logged = false;
    if (ng > 0 && !logged) {
        logged = true;
        logInfo("gas smoothing: {} volumes, blur shader {}", ng, gasBlurProg_.valid() ? "ok" : "missing");
    }
    static const bool noBlur = std::getenv("BHS_NO_GAS_BLUR") != nullptr;
    if (ng == 0 || !gasBlurProg_.valid() || noBlur) {
        for (auto& pair : gasSmooth_)
            if (pair[0]) {
                glDeleteTextures(2, pair);
                pair[0] = pair[1] = 0;
            }
        for (int& r : gasSmoothRes_) r = 0;
        return;
    }
    gasBlurProg_.use();
    gasBlurProg_.set("uSrc", 0);
    for (int i = 0; i < ng; ++i) {
        const int res = in.gas[i].resolution;
        if (res <= 0) continue;
        if (gasSmoothRes_[i] != res) {
            if (gasSmooth_[i][0]) glDeleteTextures(2, gasSmooth_[i]);
            glGenTextures(2, gasSmooth_[i]);
            for (GLuint t : gasSmooth_[i]) {
                glBindTexture(GL_TEXTURE_3D, t);
                glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, res, res, res);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            }
            gasSmoothRes_[i] = res;
        }
        gasBlurProg_.set("uRes", res);
        const GLuint src[3] = {in.gas[i].texture, gasSmooth_[i][0], gasSmooth_[i][1]};
        const GLuint dst[3] = {gasSmooth_[i][0], gasSmooth_[i][1], gasSmooth_[i][0]};
        const glm::ivec3 dir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int pass = 0; pass < 3; ++pass) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_3D, src[pass]);
            glBindImageTexture(0, dst[pass], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glUniform3i(glGetUniformLocation(gasBlurProg_.id(), "uDir"), dir[pass].x, dir[pass].y, dir[pass].z);
            const GLuint groups = static_cast<GLuint>((res + 3) / 4);
            glDispatchCompute(groups, groups, groups);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
    }
    if (std::getenv("BHS_DEBUG_TDE")) {   // debug: what the smoothed volume 0 holds (a read-back stall)
        static int dbgN = 0;
        if (++dbgN % 30 == 0 && gasSmooth_[0][0]) {
            const int res = gasSmoothRes_[0];
            std::vector<float> px(static_cast<size_t>(res) * res * res * 4);
            glBindTexture(GL_TEXTURE_3D, gasSmooth_[0][0]);
            glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_FLOAT, px.data());
            size_t filled = 0, edgeFilled = 0;
            float minK = 1e30f;
            for (int z = 0; z < res; ++z)
                for (int y = 0; y < res; ++y)
                    for (int x = 0; x < res; ++x) {
                        const float k = px[((static_cast<size_t>(z) * res + y) * res + x) * 4];
                        if (k > 0.0f) {
                            ++filled;
                            minK = std::min(minK, k);
                            if (x == 1 || y == 1 || z == 1) ++edgeFilled;
                        }
                    }
            logInfo("    smoothed volume 0: filled {} of {} (near-edge {}), min k {:.3g}", filled, px.size() / 4, edgeFilled, minK);
        }
    }
    // Peak optical depth per texel of each smoothed volume, double-buffered: this frame's pass writes
    // one buffer while last frame's results are read from the other (no stall).
    if (gasPeakProg_.valid()) {
        if (!gasPeakBuf_[0]) {
            glGenBuffers(2, gasPeakBuf_);
            for (GLuint b : gasPeakBuf_) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
                glBufferData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);
            }
        }
        const GLuint readBuf = gasPeakBuf_[gasPeakFrame_ & 1], writeBuf = gasPeakBuf_[(gasPeakFrame_ + 1) & 1];
        if (gasPeakFrame_ > 0) {
            uint32_t peaks[4] = {0, 0, 0, 0};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, readBuf);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof peaks, peaks);
            for (int i = 0; i < 4; ++i) gasLive_[i] = peaks[i] >= 2000u ? 1 : 0;   // visual weight >= 0.2 somewhere
            static int dbg = 0;
            if (std::getenv("BHS_DEBUG_TDE") && ++dbg % 30 == 0)
                logInfo("    gas peaks (tau per texel x1e4): {} {} {} {} -> live {} {} {} {}", peaks[0], peaks[1], peaks[2], peaks[3], gasLive_[0],
                        gasLive_[1], gasLive_[2], gasLive_[3]);
        }
        const uint32_t zero[4] = {0, 0, 0, 0};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, writeBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof zero, zero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, writeBuf);
        gasPeakProg_.use();
        gasPeakProg_.set("uSrc", 0);
        for (int i = 0; i < ng; ++i) {
            const int res = in.gas[i].resolution;
            if (res <= 0 || !gasSmooth_[i][0]) continue;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_3D, gasSmooth_[i][0]);
            gasPeakProg_.set("uRes", res);
            gasPeakProg_.set("uSlot", i);
            gasPeakProg_.set("uTexel", 2.0f * in.gas[i].halfSize / static_cast<float>(res));
            const GLuint groups = static_cast<GLuint>((res + 3) / 4);
            glDispatchCompute(groups, groups, groups);
        }
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        ++gasPeakFrame_;
    }
    glBindTexture(GL_TEXTURE_3D, 0);
    glUseProgram(0);
}

void Renderer::render(const FrameInput& in) {
    const Settings& s = *in.settings;
    smoothGas(in);
    updateDynamicScale(in);
    const float scale = std::clamp(s.renderScale * dynScale_, 0.1f, 2.0f);
    const int iw = std::max(16, static_cast<int>(std::lround(in.windowW * scale)));
    const int ih = std::max(16, static_cast<int>(std::lround(in.windowH * scale)));
    ensureTarget(trace_, iw, ih, GL_RGBA16F);
    ensureTarget(upscaled_, in.windowW, in.windowH, GL_RGBA16F);
    if (in.resetAccumulation) accumCount_ = 0;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    beginTimer();

    const bool accumulate = s.accumulate;
    const int cap = accumulate ? (in.diskAnimating ? std::min(s.maxAccumFrames, 6) : s.maxAccumFrames) : 1;
    const int sample = accumulate ? accumCount_ : 0;
    tracePass(in, trace_, accumulate ? jitterFor(sample) : glm::vec2(0.0f), false);
    const Target* hdrInternal = &trace_;
    if (accumulate) hdrInternal = &accumulatePass(in, trace_, sample, cap);
    upscalePass(in, *hdrInternal, upscaled_);
    bloomPass(in, upscaled_);
    updateAutoExposure(in, upscaled_);
    compositePass(in, upscaled_, in.targetFbo, in.windowW, in.windowH);

    endTimer();
    accumCount_ = std::min(accumCount_ + 1, 1 << 20);
}

bool Renderer::captureHighRes(const FrameInput& in, int width, int height, int samples, const std::string& path) {
    if (!shadersOk()) return false;
    logInfo("cinematic capture {}x{} with {} samples...", width, height, samples);
    ensureTarget(trace_, width, height, GL_RGBA16F);
    ensureTarget(upscaled_, width, height, GL_RGBA16F);
    ensureTarget(captureOut_, width, height, GL_RGBA8);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    const Target* hdr = &trace_;
    for (int sIdx = 0; sIdx < std::max(samples, 1); ++sIdx) {
        tracePass(in, trace_, jitterFor(sIdx), true);
        hdr = &accumulatePass(in, trace_, sIdx, samples);
        glFinish();
        if ((sIdx + 1) % 4 == 0 || sIdx + 1 == samples) logInfo("  sample {}/{}", sIdx + 1, samples);
    }
    upscalePass(in, *hdr, upscaled_);
    bloomPass(in, upscaled_);
    compositePass(in, upscaled_, captureOut_.fbo, width, height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, captureOut_.fbo);
    const bool ok = capture::savePngFromFramebuffer(path, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    accumCount_ = 0;
    return ok;
}

}  // namespace bhs
