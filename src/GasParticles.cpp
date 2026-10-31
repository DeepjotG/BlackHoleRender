#include "GasParticles.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "Log.h"

namespace bhs {
namespace {

// Must match gas_lens.glsl, gas_hash.glsl and gas_count.comp.
constexpr int kLensRays = 1024;
constexpr int kLensPhis = 1024;
constexpr GLsizeiptr kHashSlots = GLsizeiptr(1) << 22;
constexpr GLsizeiptr kMomSlots = GLsizeiptr(1) << 19;
constexpr GLsizeiptr kMomWords = 10;
constexpr GLsizeiptr kSplatBytes = 48;
constexpr int kReturnBins = 160;            // 1/16 decade each, from 1 M
struct CmdBlock {
    GLuint count, instances, first, base;   // glDrawArraysIndirect
    GLuint inPlay, nearHole;
    GLint lo[3], hi[3];                     // order-preserving integer images of floats
    GLuint ret[kReturnBins];
};
static_assert(sizeof(CmdBlock) == 48 + 4 * kReturnBins, "layout of DrawCmd in gas_count.comp");

float orderedToFloat(GLint i) {
    const int32_t bits = i >= 0 ? i : (i ^ 0x7FFFFFFF);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// The shadow's angular radius for a static observer at radius r (shadowAngularRadius in geodesic.glsl).
float shadowAngularRadius(float r) {
    const float s = std::min(1.0f, 5.19615242f / r * std::sqrt(std::max(1.0f - 2.0f / r, 0.0f)));
    const float a = std::asin(s);
    return r >= 3.0f ? a : 3.14159265f - a;
}

}  // namespace

bool GasParticleRenderer::load(std::string& log) {
    bool ok = true;
    const auto check = [&](ShaderProgram& p, bool loaded) {
        if (!loaded) {
            ok = false;
            log += p.log();
        }
    };
    check(lensProg_, lensProg_.loadCompute("gas_lens.comp"));
    check(countProg_, countProg_.loadCompute("gas_count.comp"));
    check(momentProg_, momentProg_.loadCompute("gas_moments.comp"));
    check(splatProg_, splatProg_.loadCompute("gas_splat.comp"));
    check(drawProg_, drawProg_.loadGraphics("gas_splat.vert", "gas_splat.frag"));
    check(resolveProg_, resolveProg_.loadGraphics("fullscreen.vert", "gas_resolve.frag"));
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!lensTex_) {
        glCreateTextures(GL_TEXTURE_2D, 1, &lensTex_);
        glTextureStorage2D(lensTex_, 1, GL_R32F, kLensRays, kLensPhis);
        glTextureParameteri(lensTex_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(lensTex_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(lensTex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(lensTex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!hashKeys_) {
        glCreateBuffers(1, &hashKeys_);
        glCreateBuffers(1, &hashCounts_);
        glNamedBufferStorage(hashKeys_, kHashSlots * 4, nullptr, GL_DYNAMIC_STORAGE_BIT);
        glNamedBufferStorage(hashCounts_, kHashSlots * 4, nullptr, GL_DYNAMIC_STORAGE_BIT);
        glCreateBuffers(1, &momKeys_);
        glCreateBuffers(1, &momVals_);
        glNamedBufferStorage(momKeys_, kMomSlots * 4, nullptr, GL_DYNAMIC_STORAGE_BIT);
        glNamedBufferStorage(momVals_, kMomSlots * kMomWords * 4, nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    if (!cmd_[0]) {
        glCreateBuffers(kRing, cmd_);
        for (GLuint b : cmd_) glNamedBufferStorage(b, sizeof(CmdBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    lensCamR_ = -1.0f;
    return ok;
}

bool GasParticleRenderer::reloadIfChanged(std::string& log) {
    bool any = false;
    for (ShaderProgram* p : {&lensProg_, &countProg_, &momentProg_, &splatProg_, &drawProg_, &resolveProg_}) {
        if (p->reloadIfChanged()) {
            any = true;
            log += p->log();
        }
    }
    if (any) lensCamR_ = -1.0f;
    return any;
}

bool GasParticleRenderer::valid() const {
    return lensProg_.valid() && countProg_.valid() && momentProg_.valid() && splatProg_.valid() && drawProg_.valid() && resolveProg_.valid();
}

void GasParticleRenderer::destroy() {
    for (ShaderProgram* p : {&lensProg_, &countProg_, &momentProg_, &splatProg_, &drawProg_, &resolveProg_}) p->destroy();
    if (lensTex_) glDeleteTextures(1, &lensTex_);
    if (hashKeys_) glDeleteBuffers(1, &hashKeys_);
    if (hashCounts_) glDeleteBuffers(1, &hashCounts_);
    if (momKeys_) glDeleteBuffers(1, &momKeys_);
    if (momVals_) glDeleteBuffers(1, &momVals_);
    if (splats_) glDeleteBuffers(1, &splats_);
    if (cmd_[0]) glDeleteBuffers(kRing, cmd_);
    for (GLsync& f : cmdFence_)
        if (f) {
            glDeleteSync(f);
            f = nullptr;
        }
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (accumTex_) glDeleteTextures(1, &accumTex_);
    if (revealTex_) glDeleteTextures(1, &revealTex_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    lensTex_ = hashKeys_ = hashCounts_ = momKeys_ = momVals_ = splats_ = fbo_ = accumTex_ = revealTex_ = vao_ = 0;
    for (GLuint& b : cmd_) b = 0;
    splatCap_ = 0;
    fw_ = fh_ = 0;
}

void GasParticleRenderer::ensureTargets(int w, int h) {
    if (fbo_ && fw_ == w && fh_ == h) return;
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (accumTex_) glDeleteTextures(1, &accumTex_);
    if (revealTex_) glDeleteTextures(1, &revealTex_);
    fw_ = w;
    fh_ = h;
    // 32-bit accumulation: thousands of overlapping splats would overflow half floats in dense gas.
    glCreateTextures(GL_TEXTURE_2D, 1, &accumTex_);
    glTextureStorage2D(accumTex_, 1, GL_RGBA32F, w, h);
    glCreateTextures(GL_TEXTURE_2D, 1, &revealTex_);
    glTextureStorage2D(revealTex_, 1, GL_R16F, w, h);
    for (GLuint t : {accumTex_, revealTex_}) {
        glTextureParameteri(t, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(t, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glCreateFramebuffers(1, &fbo_);
    glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, accumTex_, 0);
    glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT1, revealTex_, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(fbo_, 2, bufs);
    if (glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logError("gas splat framebuffer incomplete ({}x{})", w, h);
}

void GasParticleRenderer::buildLensTable(float camR) {
    if (std::abs(camR - lensCamR_) <= 2e-3f * camR) return;
    lensCamR_ = camR;
    lensProg_.use();
    lensProg_.set("uCamR", camR);
    lensProg_.set("uAlphaCrit", shadowAngularRadius(camR));
    glBindImageTexture(0, lensTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glDispatchCompute((kLensRays + 63) / 64, 1, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
}

void GasParticleRenderer::resetStats() {
    stats_ = GasParticleStats{};
    for (GLsync& f : cmdFence_)
        if (f) {
            glDeleteSync(f);
            f = nullptr;
        }
}

void GasParticleRenderer::readStats() {
    // Oldest first, so the newest finished frame wins.
    for (int k = 0; k < kRing; ++k) {
        const int s = (cmdNext_ + k) % kRing;
        if (!cmdFence_[s]) continue;
        const GLenum r = glClientWaitSync(cmdFence_[s], 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) continue;
        glDeleteSync(cmdFence_[s]);
        cmdFence_[s] = nullptr;
        CmdBlock b{};
        glGetNamedBufferSubData(cmd_[s], 0, sizeof b, &b);
        stats_.valid = true;
        stats_.inPlay = static_cast<int>(b.inPlay);
        stats_.nearHole = static_cast<int>(b.nearHole);
        if (b.inPlay > 0) {
            stats_.lo = glm::vec3(orderedToFloat(b.lo[0]), orderedToFloat(b.lo[1]), orderedToFloat(b.lo[2]));
            stats_.hi = glm::vec3(orderedToFloat(b.hi[0]), orderedToFloat(b.hi[1]), orderedToFloat(b.hi[2]));
        }
        stats_.timeM = cmdTime_[s];
        stats_.returnInM = -1.0;
        double cum = 0.0;
        for (int i = 0; i < kReturnBins; ++i) {
            if (cum + b.ret[i] >= cmdReturnCount_[s] && b.ret[i] > 0) {
                const double f = (cmdReturnCount_[s] - cum) / b.ret[i];
                stats_.returnInM = std::pow(10.0, (i + std::clamp(f, 0.0, 1.0)) / 16.0);
                break;
            }
            cum += b.ret[i];
        }
    }
}

void GasParticleRenderer::draw(const GasParticlesRef& gas, const GasDrawParams& view, GLuint dstFbo, int w, int h) {
    if (!valid() || gas.count <= 0 || w <= 0 || h <= 0) return;
    // BHS_GAS_TIMING: GPU time of each pass (glFinish between them), averaged over 120 frames.
    static const bool timing = std::getenv("BHS_GAS_TIMING") != nullptr;
    static double acc[8] = {};
    static int timedFrames = 0;
    std::chrono::steady_clock::time_point tPrev;
    int stage = 0;
    const auto mark = [&]() {
        if (!timing) return;
        glFinish();
        const auto now = std::chrono::steady_clock::now();
        if (stage > 0) acc[stage - 1] += std::chrono::duration<double, std::milli>(now - tPrev).count();
        tPrev = now;
        ++stage;
    };
    mark();
    readStats();
    const int slot = cmdNext_;
    cmdNext_ = (cmdNext_ + 1) % kRing;
    if (cmdFence_[slot]) {   // not read back yet: that sample is dropped
        glDeleteSync(cmdFence_[slot]);
        cmdFence_[slot] = nullptr;
    }

    const float camR = std::max(glm::length(view.camPos), 2.05f);
    const float alphaCrit = shadowAngularRadius(camR);
    if (view.lensing) buildLensTable(camR);
    mark();   // 1 lens table

    const GLuint zero = 0;
    glClearNamedBufferData(hashKeys_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glClearNamedBufferData(hashCounts_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glClearNamedBufferData(momKeys_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glClearNamedBufferData(momVals_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    const size_t need = static_cast<size_t>(gas.count) * 2;   // a primary and a secondary image each
    if (need > splatCap_) {
        if (splats_) glDeleteBuffers(1, &splats_);
        glCreateBuffers(1, &splats_);
        glNamedBufferStorage(splats_, static_cast<GLsizeiptr>(need) * kSplatBytes, nullptr, 0);
        splatCap_ = need;
    }
    const CmdBlock init{4, 0, 0, 0, 0, 0, {INT_MAX, INT_MAX, INT_MAX}, {INT_MIN, INT_MIN, INT_MIN}, {}};
    glNamedBufferSubData(cmd_[slot], 0, sizeof init, &init);
    cmdTime_[slot] = gas.timeM;
    cmdReturnCount_[slot] = std::max(1.0f, gas.returnFraction * static_cast<float>(gas.count));

    glBindBufferRange(GL_UNIFORM_BUFFER, 3, gas.params, gas.paramsOffset, gas.paramsSize);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gas.pos);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gas.vel);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gas.aux);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, momKeys_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, momVals_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, hashKeys_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, hashCounts_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, splats_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, cmd_[slot]);
    // The finest density cell holds about a dozen particles in the core of the intact star.
    const float cell0 = std::max(gas.starRadius, 1e-6f) / 64.0f;
    mark();   // 2 clears and bindings

    countProg_.use();
    countProg_.set("uCount", gas.count);
    countProg_.set("uNearR", gas.nearRadius);
    countProg_.set("uFarR", gas.farRadius);
    countProg_.set("uCell0", cell0);
    glDispatchCompute(static_cast<GLuint>((gas.count + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    mark();   // 3 counts

    momentProg_.use();
    momentProg_.set("uCount", gas.count);
    momentProg_.set("uCell0", cell0);
    glDispatchCompute(static_cast<GLuint>((gas.count + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    mark();   // 4 moments

    splatProg_.use();
    splatProg_.set("uCount", gas.count);
    splatProg_.set("uPartTotal", static_cast<float>(gas.count));
    splatProg_.set("uCell0", cell0);
    splatProg_.set("uCamPos", view.camPos);
    splatProg_.set("uCamBasis", view.camBasis);
    splatProg_.set("uTanHalfFov", view.tanHalfFov);
    splatProg_.set("uResolution", glm::vec2(w, h));
    splatProg_.set("uLensing", view.lensing ? 1 : 0);
    splatProg_.set("uAlphaCrit", alphaCrit);
    splatProg_.set("uGasBrightness", view.brightness);
    splatProg_.set("uGasOpacity", view.opacity);
    splatProg_.set("uRefLen", gas.refLen);
    splatProg_.set("uDiskFade", glm::vec3(view.diskOuter, view.diskSlab, view.diskPresence));
    splatProg_.set("uMaxSplats", static_cast<unsigned>(splatCap_));
    splatProg_.set("uMaxExtent", 0.25f * static_cast<float>(h));
    splatProg_.set("uLens", 0);
    splatProg_.set("uBlackbodyLut", 1);
    glBindTextureUnit(0, lensTex_);
    glBindTextureUnit(1, view.blackbodyLut);
    glDispatchCompute(static_cast<GLuint>((gas.count + 127) / 128), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    mark();   // 5 splat generation

    ensureTargets(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    const float clearAccum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float clearReveal[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, clearAccum);
    glClearBufferfv(GL_COLOR, 1, clearReveal);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE, GL_ONE);
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
    drawProg_.use();
    drawProg_.set("uResolution", glm::vec2(w, h));
    glBindVertexArray(vao_);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_[slot]);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    cmdFence_[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    mark();   // 6 splat drawing

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, w, h);
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    resolveProg_.use();
    resolveProg_.set("uAccum", 0);
    resolveProg_.set("uReveal", 1);
    glBindTextureUnit(0, accumTex_);
    glBindTextureUnit(1, revealTex_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);

    glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
    for (GLuint b : {0u, 1u, 2u, 9u, 10u, 11u, 12u, 13u, 14u}) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0);
    glBindTextureUnit(0, 0);
    glBindTextureUnit(1, 0);
    glBindVertexArray(0);
    mark();   // 7 resolve
    if (timing && ++timedFrames % 120 == 0) {
        logInfo("gas passes (ms, mean of 120 frames): lens {:.2f} clear {:.2f} count {:.2f} moments {:.2f} splat {:.2f} draw {:.2f} resolve {:.2f}",
                acc[0] / 120, acc[1] / 120, acc[2] / 120, acc[3] / 120, acc[4] / 120, acc[5] / 120, acc[6] / 120);
        for (double& a : acc) a = 0.0;
    }
}

}  // namespace bhs
