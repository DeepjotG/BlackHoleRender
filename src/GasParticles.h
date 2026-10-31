#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <string>

#include "Shader.h"

namespace bhs {

// The star-disruption particles as the renderer needs them (see tde::TdeSim::ParticleView).
struct GasParticlesRef {
    GLuint pos = 0, vel = 0, aux = 0, params = 0;
    GLintptr paramsOffset = 0;
    GLsizeiptr paramsSize = 0;
    int count = 0;              // 0: nothing to draw
    float starRadius = 0.0f;    // M: the finest density cell is a fraction of it
    float nearRadius = 0.0f;    // M: stats().nearHole counts the particles within this distance of the hole
    float refLen = 1.0f;        // M: the reference length of gas.glsl (dense = optically thick over it)
    double timeM = 0.0;         // event clock of this frame (stats are tagged with it)
    float farRadius = 1e30f;    // M: bound gas farther out predicts when it comes back
    float returnFraction = 0.0025f;   // of all particles: stats().returnInM is when this much is back
};

// Measured while drawing; read back a few frames late (valid = false until the first read-back).
struct GasParticleStats {
    bool valid = false;
    int inPlay = 0;             // particles still drawn (not accreted, not escaped)
    int nearHole = 0;           // ... within GasParticlesRef::nearRadius of the hole
    glm::vec3 lo{0.0f}, hi{0.0f};   // their bounding box (M)
    double timeM = 0.0;         // event clock of the measured frame
    double returnInM = -1.0;    // time from then until GasParticlesRef::returnFraction of the star is back
                                // at pericentre, from the gas beyond farRadius (-1: not enough of it)
};

struct GasDrawParams {
    glm::vec3 camPos{0.0f};
    glm::mat3 camBasis{1.0f};   // columns: right, up, back
    float tanHalfFov = 0.5f;
    bool lensing = true;
    float brightness = 1.0f, opacity = 1.0f;
    GLuint blackbodyLut = 0;
    float diskOuter = 0.0f, diskSlab = 0.0f, diskPresence = 0.0f;   // the disk forming from the gas (see gas_splat.comp)
};

// Draws the star-disruption gas as its particles: each one a soft splat at full screen resolution, at
// the places the black hole's lensing shows it (gas_lens.comp, gas_count.comp, gas_splat.comp, then
// order-independent compositing over the HDR image). gas_moments.comp shapes each particle's splat.
class GasParticleRenderer {
public:
    bool load(std::string& log);
    bool reloadIfChanged(std::string& log);
    bool valid() const;
    void destroy();
    // Composites the gas over the HDR image in dstFbo (w x h).
    void draw(const GasParticlesRef& gas, const GasDrawParams& view, GLuint dstFbo, int w, int h);
    const GasParticleStats& stats() const { return stats_; }
    void resetStats();   // a new event: forget the last one's measurements

private:
    void ensureTargets(int w, int h);
    void buildLensTable(float camR);
    void readStats();

    ShaderProgram lensProg_, countProg_, momentProg_, splatProg_, drawProg_, resolveProg_;
    GLuint lensTex_ = 0;
    float lensCamR_ = -1.0f;
    GLuint hashKeys_ = 0, hashCounts_ = 0, momKeys_ = 0, momVals_ = 0;
    GLuint splats_ = 0;
    size_t splatCap_ = 0;
    static constexpr int kRing = 3;
    GLuint cmd_[kRing] = {};
    GLsync cmdFence_[kRing] = {};
    double cmdTime_[kRing] = {};
    float cmdReturnCount_[kRing] = {};
    int cmdNext_ = 0;
    GLuint fbo_ = 0, accumTex_ = 0, revealTex_ = 0;
    int fw_ = 0, fh_ = 0;
    GLuint vao_ = 0;
    GasParticleStats stats_;
};

}  // namespace bhs
