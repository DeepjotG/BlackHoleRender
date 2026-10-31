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
// length `chord`, sampled by n particles of kernel radius h, adds up to tau.
double tauPerParticle(double tau, double volume, double chord, int n, double h) {
    const double spacing = std::cbrt(std::max(volume, 1e-300) / std::max(n, 1));
    const double layers = std::max((chord / spacing) * kPi * (h / spacing) * (h / spacing), 1.0);
    return tau / layers;
}

}  // namespace

bool GrbEvent::init() {
    if (!prog_.loadCompute("grb_particles.comp")) {
        logWarn("neutron-star merger: particle shader failed to compile:\n{}", prog_.log());
        return false;
    }
    count_ = kCountWisp + 2 * kCountJet + 2 * kCountHalo;
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
    // The spines relative to the merger site (double precision there; a jet is millimetres of a metre long
    // in these units when it launches).
    std::vector<glm::vec4> sp(2 * kSpinePoints);
    for (int w = 0; w < 2; ++w)
        for (int k = 0; k < kSpinePoints; ++k) {
            const glm::dvec4 q = model_.spine(w).p[static_cast<size_t>(k)];
            sp[static_cast<size_t>(w * kSpinePoints + k)] = glm::vec4(glm::vec3(glm::dvec3(q) - model_.mergeSite()), static_cast<float>(q.w));
        }
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
    const double tjs = std::max((s.tauM - prm.jetDelaySeconds / model_.secondsPerM()) * model_.secondsPerM(), 0.0);
    // Wisps: fingers in clumps (about 60% of the directions) reaching up to a quarter of the radius beyond
    // the photosphere, optically thin.
    const double R = std::max(s.knRadiusM, 1e-12);
    const double wispVol = 4.0 / 3.0 * kPi * (std::pow(1.25 * R, 3.0) - R * R * R) * 0.6;
    const int wispN = static_cast<int>(kCountWisp * 0.6);
    const float wispKernel = static_cast<float>(0.9 * std::cbrt(wispVol / wispN));
    const float tauWisp = static_cast<float>(tauPerParticle(0.6 * std::min(s.knTau, 1.0), wispVol, 0.25 * R, wispN, wispKernel));
    // Jets: a bright core inside a wider cocoon, both optically thin enough to glow through.
    const double sinCore = std::sin(glm::radians(prm.jetHalfAngleDeg)), sinCocoon = std::sin(glm::radians(prm.cocoonHalfAngleDeg));
    const double hj = std::max(std::max(s.jetHeadM[0], s.jetHeadM[1]), 1e-12), hc = std::max(std::max(s.cocoonHeadM[0], s.cocoonHeadM[1]), 1e-12);
    const double coreVol = kPi / 3.0 * (hj * sinCore) * (hj * sinCore) * hj;
    const double cocoonVol = kPi / 3.0 * (hc * sinCocoon) * (hc * sinCocoon) * hc;
    const double coreKernel = 0.45 * 0.6 * hj * sinCore, cocoonKernel = 0.6 * 0.6 * hc * sinCocoon;   // at mid-length
    // Their splats are drawn out along the jet (2.2 and 1.5 times: grb_particles.comp), covering that much more.
    const float tauCore = static_cast<float>(tauPerParticle(2.0, coreVol, 1.2 * hj * sinCore, kCoreCount, coreKernel) / 2.2);
    const float tauCocoon = static_cast<float>(tauPerParticle(0.5, cocoonVol, 1.2 * hc * sinCocoon, kCountJet - kCoreCount, cocoonKernel) / 1.5);
    // Afterglow haloes round the jets' heads: large, faint, fading as the shock slows.
    const double haloR = 0.3 * hj;
    const double haloVol = 4.0 / 3.0 * kPi * haloR * haloR * haloR;
    const float tauHalo = static_cast<float>(tauPerParticle(0.3, haloVol, 2.0 * haloR, kCountHalo, 0.5 * haloR));
    const float haloBright = static_cast<float>(0.8 / std::pow(1.0 + tjs / 120.0, 0.8));
    prog_.use();
    prog_.set("uCountWisp", kCountWisp);
    prog_.set("uCountJet", kCountJet);
    prog_.set("uCountHalo", kCountHalo);
    prog_.set("uCoreCount", kCoreCount);
    prog_.set("uSiteOffset", glm::vec3(model_.mergeSite() - s.centre));
    prog_.set("uKn", glm::vec4(static_cast<float>(s.knRadiusM), tauWisp, 0.3f, wispKernel));
    prog_.set("uKnTemp", glm::vec2(static_cast<float>(s.knTempK * 1e-4), static_cast<float>(s.knTempEqK * 1e-4)));
    prog_.set("uAxis", glm::vec3(s.axis));
    prog_.set("uHeads", glm::vec4(static_cast<float>(s.jetHeadM[0]), static_cast<float>(s.cocoonHeadM[0]), static_cast<float>(s.jetHeadM[1]),
                                  static_cast<float>(s.cocoonHeadM[1])));
    prog_.set("uJet", glm::vec4(static_cast<float>(std::cos(glm::radians(prm.jetHalfAngleDeg))),
                                static_cast<float>(std::cos(glm::radians(prm.cocoonHalfAngleDeg))), tauCore, tauCocoon));
    prog_.set("uJetTemp", glm::vec2(static_cast<float>(s.jetTempK * 1e-4), static_cast<float>(s.cocoonTempK * 1e-4)));
    prog_.set("uJetBright", glm::vec2(static_cast<float>(s.jetBright), static_cast<float>(s.cocoonBright)));
    prog_.set("uJetKernel", glm::vec3(0.45f, 0.6f, 0.35f));
    prog_.set("uHalo", glm::vec4(0.3f, tauHalo, haloBright, 0.5f));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufPos_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufVel_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufAux_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, bufSpines_);
    glDispatchCompute(static_cast<GLuint>((count_ + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    for (GLuint b : {0u, 1u, 2u, 15u}) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0);
    glUseProgram(0);
    // The renderer's finest density cell: a few hundredths of the largest structure.
    cellScale_ = static_cast<float>(std::max(std::max(1.25 * s.knRadiusM, 0.25 * hj), 1e-9));
}

GasParticlesRef GrbEvent::particles() const {
    GasParticlesRef ref;
    if (!ready_ || !merged()) return ref;
    const Params& prm = model_.params();
    ref.pos = bufPos_;
    ref.vel = bufVel_;
    ref.aux = bufAux_;
    ref.params = bufParams_;
    ref.paramsOffset = 0;
    ref.paramsSize = sizeof(ParamsBlock);
    ref.count = count_;
    ref.starRadius = cellScale_;    // the finest cell is this / 64
    ref.law = 1;
    ref.origin = state_.centre;
    ref.flowM = static_cast<float>(std::max(state_.tauM - prm.jetDelaySeconds / model_.secondsPerM(), 0.0));
    ref.wantStats = false;
    return ref;
}

void GrbEvent::appendBodies(std::vector<BodyDesc>& bodies, float brightness) const {
    if (!active_) return;
    if (!state_.merged) {
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
        return;
    }
    const double ts = state_.tauM * model_.secondsPerM();
    // The kilonova's photosphere: an exploding, boiling sphere (big plumes, limb darkening, at the ejecta's
    // temperature) while the ejecta are optically thick, flashing at the merger; it fades and turns
    // transparent as they thin out.
    if (state_.knTau > 0.25 && state_.knRadiusM > 0.0) {
        const double thick = std::clamp((state_.knTau - 0.25) / 1.25, 0.0, 1.0);
        BodyDesc b;
        b.name = "Kilonova";
        b.kind = BodyKind::Star;
        b.pos = state_.centre;
        b.vel = state_.siteVel;
        b.radius = state_.knRadiusM;
        b.teff = static_cast<float>(state_.knTempK);
        b.lightIntensity = static_cast<float>(0.6 * (1.0 + 6.0 * std::exp(-ts / 2.0)) * thick);
        b.atmosphere = 4.0f;           // plumes per radius
        b.turbulence = 0.85f;
        b.phase = static_cast<float>(std::log(1.0 + ts / 0.5));
        b.opacity = static_cast<float>(thick);
        b.seed = 21;
        b.collidable = false;
        b.info = "The ejecta's glowing surface, expanding at a fifth of the speed of light";
        bodies.push_back(b);
    }
    // The engine: the merger remnant and its torus, a brilliant point for the first minutes.
    if (ts < 240.0) {
        BodyDesc b;
        b.name = "Merger remnant";
        b.kind = BodyKind::Star;
        b.pos = state_.centre;
        b.vel = state_.siteVel;
        b.radius = 80e3 / model_.metersPerM();
        b.teff = 40000.0f;
        b.lightIntensity = static_cast<float>(4.0 * std::exp(-ts / 50.0));
        b.seed = 23;
        b.collidable = false;
        b.info = "What the two stars became, powering the jets";
        bodies.push_back(b);
    }
}

}  // namespace bhs::grb
