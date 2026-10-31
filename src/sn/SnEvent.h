#pragma once
// The supernova on the GPU: runs the model (SnModel.h) on the event's own clock, provides the star / the
// flash / the expanding envelope and the pulsar as bodies for the ray tracer, and places the wisps and
// the remnant's particles (shaders/sn_particles.comp) for the gas splat renderer.
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <vector>

#include "Bodies.h"
#include "GasParticles.h"
#include "Shader.h"
#include "sn/SnModel.h"

namespace bhs::sn {

class SnEvent {
public:
    static constexpr int kCountWisp = 6144;
    static constexpr int kCountFil = 20480;
    static constexpr int kCountShell = 3072;
    static constexpr int kCountPwn = 768;

    bool init();
    void shutdown();
    void spawn(const Params& prm);
    void clear() { active_ = false; }
    bool active() const { return active_; }
    void step(double dtM);
    const State& state() const { return state_; }
    const Model& model() const { return model_; }
    double timeM() const { return timeM_; }
    GasParticlesRef particles() const;   // count 0 while nothing but the star is there
    void appendBodies(std::vector<BodyDesc>& bodies) const;

private:
    void updateParticles();

    bool ready_ = false, active_ = false;
    double timeM_ = 0.0;
    Model model_;
    State state_;
    ShaderProgram prog_;
    GLuint bufPos_ = 0, bufVel_ = 0, bufAux_ = 0, bufParams_ = 0;
    int count_ = 0;
    float cellScale_ = 1.0f;
};

}  // namespace bhs::sn
