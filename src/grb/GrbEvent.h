#pragma once
// The neutron-star merger on the GPU: runs the model (GrbModel.h) on the event's own clock, places the
// kilonova and jet particles (shaders/grb_particles.comp) for the gas splat renderer, and provides the
// two stars as bodies for the ray tracer before they merge.
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <vector>

#include "Bodies.h"
#include "GasParticles.h"
#include "Shader.h"
#include "grb/GrbModel.h"

namespace bhs::grb {

class GrbEvent {
public:
    static constexpr int kCountWisp = 6144;   // the kilonova's ragged outer layer (its photosphere is a body)
    static constexpr int kCountJet = 8192;    // per jet
    static constexpr int kCoreCount = 6144;   // of each jet; the rest is the cocoon
    static constexpr int kCountHalo = 160;    // afterglow particles per jet

    bool init();                 // compiles the particle shader and creates the buffers; false: particles unavailable
    void shutdown();
    void spawn(const Params& prm);
    void clear() { active_ = false; }
    bool active() const { return active_; }
    void step(double dtM);       // advances the event clock and updates the particles
    const State& state() const { return state_; }
    const Model& model() const { return model_; }
    double timeM() const { return timeM_; }
    bool merged() const { return active_ && state_.merged; }
    // The particle set for the renderer (after the merger; count 0 before).
    GasParticlesRef particles() const;
    // The two stars before the merger; afterwards the kilonova's photosphere and the merger remnant.
    void appendBodies(std::vector<BodyDesc>& bodies, float brightness) const;

private:
    void updateParticles();

    bool ready_ = false, active_ = false;
    double timeM_ = 0.0;
    Model model_;
    State state_;
    ShaderProgram prog_;
    GLuint bufPos_ = 0, bufVel_ = 0, bufAux_ = 0, bufSpines_ = 0, bufParams_ = 0;
    int count_ = 0;
    float cellScale_ = 1.0f;     // the renderer's finest density cell is derived from it
};

}  // namespace bhs::grb
