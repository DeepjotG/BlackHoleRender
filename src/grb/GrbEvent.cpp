#include "grb/GrbEvent.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Log.h"

namespace bhs::grb {
namespace {

// The uniform block the gas splat shaders bind at 3 (shaders/tde_params.glsl): the star disruption's
// parameters. The merger's particles need none of them (all are state 0, their shading is law 1), but
// the block must exist; it is zero apart from the particle count.
struct ParamsBlock {
    glm::vec4 gridDep[4], gridFld[4];
    glm::vec4 ramp, event, step, mono, scale, phys, limit, vis, poly, extra, frozen, frozenVel;
    glm::ivec4 info;
};
static_assert(sizeof(ParamsBlock) == 21 * 16, "layout of TdeParams in shaders/tde_params.glsl");

constexpr double kPi = 3.14159265358979;

// Optical depth per particle so that a line of sight through a structure of volume V, crossed over a
// length `chord`, sampled by n particles, adds up to tau: the splats overlap ~4.5 (chord / spacing) deep.
double tauPerParticle(double tau, double volume, double chord, int n) {
    const double spacing = std::cbrt(std::max(volume, 1e-300) / std::max(n, 1));
    const double layers = std::max(4.5 * chord / std::max(spacing, 1e-300), 1.0);
    return tau / layers;
}

}  // namespace

bool GrbEvent::init() {
    if (!prog_.loadCompute("grb_particles.comp")) {
        logWarn("neutron-star merger: particle shader failed to compile:\n{}", prog_.log());
        return false;
    }
    count_ = kCountKn + 2 * kCountJet;
    glCreateBuffers(1, &bufPos_);
    glCreateBuffers(1, &bufVel_);
    glCreateBuffers(1, &bufAux_);
    for (GLuint b : {bufPos_, bufVel_, bufAux_}) glNamedBufferStorage(b, static_cast<GLsizeiptr>(count_) * 16, nullptr, 0);
    glCreateBuffers(1, &bufSpines_);
    glNamedBufferStorage(bufSpines_, 2 * kSpinePoints * 16, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateBuffers(1, &bufParams_);
    glNamedBufferStorage(bufParams_, sizeof(ParamsBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    ready_ = true;
    return true;
}

void GrbEvent::shutdown() {
    prog_.destroy();
    for (GLuint* b : {&bufPos_, &bufVel_, &bufAux_, &bufSpines_, &bufParams_})
        if (*b) {
            glDeleteBuffers(1, b);
            *b = 0;
        }
    ready_ = active_ = false;
}

void GrbEvent::spawn(const Params& prm) {
    model_.init(prm);
    timeM_ = 0.0;
    state_ = model_.at(0.0);
    active_ = true;
    if (!ready_) return;
    std::vector<glm::vec4> sp(2 * kSpinePoints);
    for (int w = 0; w < 2; ++w)
        for (int k = 0; k < kSpinePoints; ++k) sp[static_cast<size_t>(w * kSpinePoints + k)] = glm::vec4(model_.spine(w).p[static_cast<size_t>(k)]);
    glNamedBufferSubData(bufSpines_, 0, static_cast<GLsizeiptr>(sp.size() * sizeof(glm::vec4)), sp.data());
    ParamsBlock pb{};
    pb.info = glm::ivec4(count_, 2, 0, 0);
    pb.scale = glm::vec4(1.0f, 1.0f, 1.0f, static_cast<float>(count_));
    glNamedBufferSubData(bufParams_, 0, sizeof pb, &pb);
    logInfo("neutron-star merger: {} km apart, merging in {:.2f} s; jet 0 passes the hole at {:.1f} M{}, jet 1 at {:.1f} M{}",
            prm.startSeparationKm, model_.mergeTimeM() * model_.secondsPerM(), model_.spine(0).minR,
            model_.spine(0).captured ? " (captured)" : "", model_.spine(1).minR, model_.spine(1).captured ? " (captured)" : "");
}

void GrbEvent::step(double dtM) {
    if (!active_) return;
    timeM_ += std::max(dtM, 0.0);
    state_ = model_.at(timeM_);
    if (ready_ && state_.merged) updateParticles();
}

void GrbEvent::updateParticles() {
    const State& s = state_;
    const Params& prm = model_.params();
    // Per-particle optical depths from the structures' sizes (see tauPerParticle).
    const double R = std::max(s.knRadiusM, 1e-12);
    const double rin = (1.0 - s.knShellFrac) * R;
    const double shellVol = 4.0 / 3.0 * kPi * (R * R * R - rin * rin * rin);
    const float tauKn = static_cast<float>(tauPerParticle(s.knTau, shellVol, s.knShellFrac * R, kCountKn));
    const double tanCore = std::tan(glm::radians(prm.jetHalfAngleDeg)), tanCocoon = std::tan(glm::radians(prm.cocoonHalfAngleDeg));
    const double hj = std::max(std::max(s.jetHeadM[0], s.jetHeadM[1]), 1e-12), hc = std::max(std::max(s.cocoonHeadM[0], s.cocoonHeadM[1]), 1e-12);
    const double coreVol = kPi / 3.0 * (hj * tanCore) * (hj * tanCore) * hj;
    const double cocoonVol = kPi / 3.0 * (hc * tanCocoon) * (hc * tanCocoon) * hc;
    const float tauCore = static_cast<float>(tauPerParticle(1.2, coreVol, 0.7 * hj * tanCore, kCoreCount));
    const float tauCocoon = static_cast<float>(tauPerParticle(0.2, cocoonVol, 0.7 * hc * tanCocoon, kCountJet - kCoreCount));
    prog_.use();
    prog_.set("uCountKn", kCountKn);
    prog_.set("uCountJet", kCountJet);
    prog_.set("uCoreCount", kCoreCount);
    prog_.set("uCentre", glm::vec3(s.centre));
    prog_.set("uKn", glm::vec4(static_cast<float>(s.knRadiusM), static_cast<float>(s.knShellFrac), tauKn, static_cast<float>(s.knBright)));
    prog_.set("uKnTemp", glm::vec2(static_cast<float>(s.knTempK * 1e-4), static_cast<float>(s.knTempEqK * 1e-4)));
    prog_.set("uAxis", glm::vec3(s.axis));
    prog_.set("uHeads", glm::vec4(static_cast<float>(s.jetHeadM[0]), static_cast<float>(s.cocoonHeadM[0]), static_cast<float>(s.jetHeadM[1]),
                                  static_cast<float>(s.cocoonHeadM[1])));
    prog_.set("uJet", glm::vec4(static_cast<float>(std::cos(glm::radians(prm.jetHalfAngleDeg))),
                                static_cast<float>(std::cos(glm::radians(prm.cocoonHalfAngleDeg))), tauCore, tauCocoon));
    prog_.set("uJetTemp", glm::vec2(static_cast<float>(s.jetTempK * 1e-4), static_cast<float>(s.cocoonTempK * 1e-4)));
    prog_.set("uJetBright", glm::vec2(static_cast<float>(s.jetBright), static_cast<float>(s.cocoonBright)));
    prog_.set("uEjectaSpeed", static_cast<float>(prm.ejectaSpeed));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufPos_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufVel_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufAux_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, bufSpines_);
    glDispatchCompute(static_cast<GLuint>((count_ + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    for (GLuint b : {0u, 1u, 2u, 15u}) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0);
    glUseProgram(0);
    // The renderer's finest density cell: a few hundredths of the largest structure.
    cellScale_ = static_cast<float>(std::max(std::max(s.knRadiusM, 0.25 * hj), 1e-9));
}

GasParticlesRef GrbEvent::particles() const {
    GasParticlesRef ref;
    if (!ready_ || !merged()) return ref;
    ref.pos = bufPos_;
    ref.vel = bufVel_;
    ref.aux = bufAux_;
    ref.params = bufParams_;
    ref.paramsOffset = 0;
    ref.paramsSize = sizeof(ParamsBlock);
    ref.count = count_;
    ref.starRadius = cellScale_;    // the finest cell is this / 64
    ref.law = 1;
    ref.centre = glm::vec3(state_.centre);
    ref.wantStats = false;
    return ref;
}

void GrbEvent::appendBodies(std::vector<BodyDesc>& bodies, float brightness) const {
    if (!active_ || state_.merged) return;
    for (int k = 0; k < 2; ++k) {
        BodyDesc b;
        b.name = k == 0 ? "Neutron star A" : "Neutron star B";
        b.kind = BodyKind::Star;
        b.pos = k == 0 ? state_.star1 : state_.star2;
        b.vel = k == 0 ? state_.vel1 : state_.vel2;
        b.radius = model_.nsRadiusM();
        b.teff = 40000.0f;             // a million-degree surface; white-blue in the visible
        b.lightIntensity = 0.75f * brightness;   // the ordinary star law (bodies.glsl): its colour at this brightness
        b.seed = 11u + static_cast<uint32_t>(k);
        b.info = "1.4 solar masses in a 24 km ball, spiralling in";
        bodies.push_back(b);
    }
}

}  // namespace bhs::grb
