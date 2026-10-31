#include "sn/SnEvent.h"

#include <algorithm>
#include <cmath>

#include "Log.h"

namespace bhs::sn {
namespace {

// The uniform block the gas splat shaders bind at 3 (shaders/tde_params.glsl); zero apart from the
// particle count (see GrbEvent.cpp).
struct ParamsBlock {
    glm::vec4 gridDep[4], gridFld[4];
    glm::vec4 ramp, event, step, mono, scale, phys, limit, vis, poly, extra, frozen, frozenVel;
    glm::ivec4 info;
};
static_assert(sizeof(ParamsBlock) == 21 * 16, "layout of TdeParams in shaders/tde_params.glsl");

constexpr double kPi = 3.14159265358979;

// Optical depth per particle so that a line of sight through a structure of volume V, crossed over a
// length `chord`, sampled by n particles of kernel radius h, adds up to tau.
double tauPerParticle(double tau, double volume, double chord, double n, double h) {
    const double spacing = std::cbrt(std::max(volume, 1e-300) / std::max(n, 1.0));
    const double layers = std::max((chord / spacing) * kPi * (h / spacing) * (h / spacing), 1.0);
    return tau / layers;
}

constexpr float kFilThreshold = 0.9f;    // the ridges of the filament noise: about a quarter of the candidates
constexpr double kFilAccept = 0.25;
constexpr double kWispAccept = 0.6;      // the clumps of the ejecta fingers

}  // namespace

bool SnEvent::init() {
    if (!prog_.loadCompute("sn_particles.comp")) {
        logWarn("supernova: particle shader failed to compile:\n{}", prog_.log());
        return false;
    }
    count_ = kCountWisp + kCountFil + kCountShell + kCountPwn;
    glCreateBuffers(1, &bufPos_);
    glCreateBuffers(1, &bufVel_);
    glCreateBuffers(1, &bufAux_);
    for (GLuint b : {bufPos_, bufVel_, bufAux_}) glNamedBufferStorage(b, static_cast<GLsizeiptr>(count_) * 16, nullptr, 0);
    glCreateBuffers(1, &bufParams_);
    glNamedBufferStorage(bufParams_, sizeof(ParamsBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    ready_ = true;
    return true;
}

void SnEvent::shutdown() {
    prog_.destroy();
    for (GLuint* b : {&bufPos_, &bufVel_, &bufAux_, &bufParams_})
        if (*b) {
            glDeleteBuffers(1, b);
            *b = 0;
        }
    ready_ = active_ = false;
}

void SnEvent::spawn(const Params& prm) {
    model_.init(prm);
    timeM_ = 0.0;
    state_ = model_.at(0.0);
    active_ = true;
    if (!ready_) return;
    ParamsBlock pb{};
    pb.info = glm::ivec4(count_, 2, 0, 0);
    pb.scale = glm::vec4(1.0f, 1.0f, 1.0f, static_cast<float>(count_));
    glNamedBufferSubData(bufParams_, 0, sizeof pb, &pb);
    logInfo("supernova: a {} solar-radius star; the shock breaks out {:.1f} h after the collapse", prm.starRadiusSolar,
            model_.crossingSeconds() / 3600.0);
}

void SnEvent::step(double dtM) {
    if (!active_) return;
    timeM_ += std::max(dtM, 0.0);
    state_ = model_.at(timeM_);
    if (ready_ && state_.breakout) updateParticles();
}

void SnEvent::updateParticles() {
    const State& s = state_;
    // Ejecta fingers beyond the photosphere (1 to about 1.3 of its radius) while the envelope is opaque.
    const double Re = std::max(s.envelopeRadiusM, 1e-12);
    const double wispVol = 4.0 / 3.0 * kPi * (std::pow(1.3 * Re, 3.0) - Re * Re * Re) * kWispAccept;
    const double wispN = kCountWisp * kWispAccept;
    const float wispKernel = static_cast<float>(0.9 * std::cbrt(wispVol / wispN));
    const float tauWisp = static_cast<float>(tauPerParticle(0.28, wispVol, 0.3 * Re, wispN, wispKernel));
    const float wispBright = static_cast<float>(0.5 * s.photoOpacity * std::min(s.photoBright, 3.0));
    // The fingers grow as the ejecta do (Rayleigh-Taylor plumes take days to develop).
    const float wispReach = static_cast<float>(std::clamp(0.5 * std::log10(1.0 + s.tauS / model_.flashSeconds()), 0.2, 1.0));
    // The remnant: filaments in a thick shell, the forward shock's rim, the wind nebula.
    const double Rn = std::max(s.nebulaRadiusM, 1e-12);
    const double filVol = 4.0 / 3.0 * kPi * (Rn * Rn * Rn - std::pow(0.72 * Rn, 3.0)) * kFilAccept;
    const double filN = kCountFil * kFilAccept;
    const float filKernel = static_cast<float>(0.85 * std::cbrt(filVol / filN));
    const float tauFil = static_cast<float>(tauPerParticle(0.5, filVol, 0.56 * Rn, filN, filKernel));
    const double Rs = std::max(s.shellRadiusM, 1e-12);
    const double shellVol = 4.0 * kPi * Rs * Rs * (0.05 * Rs);
    const float shellKernel = static_cast<float>(1.4 * std::cbrt(shellVol / kCountShell));
    const float tauShell = static_cast<float>(tauPerParticle(0.15, shellVol, 0.3 * Rs, kCountShell, shellKernel));
    const double Rp = 0.16 * Rn;
    const double pwnVol = 4.0 / 3.0 * kPi * Rp * Rp * Rp;
    const float pwnKernel = static_cast<float>(1.5 * std::cbrt(pwnVol / kCountPwn));
    const float tauPwn = static_cast<float>(tauPerParticle(0.35, pwnVol, 1.5 * Rp, kCountPwn, pwnKernel));
    prog_.use();
    prog_.set("uCountWisp", kCountWisp);
    prog_.set("uCountFil", kCountFil);
    prog_.set("uCountShell", kCountShell);
    prog_.set("uCountPwn", kCountPwn);
    prog_.set("uWisp", glm::vec4(static_cast<float>(s.envelopeRadiusM), tauWisp, wispBright, wispKernel));
    prog_.set("uWispTemp", static_cast<float>(s.photoTempK * 1e-4));
    prog_.set("uNeb", glm::vec4(static_cast<float>(s.nebulaRadiusM), tauFil, static_cast<float>(0.6 * s.nebulaBright), filKernel));
    prog_.set("uShell", glm::vec4(static_cast<float>(s.shellRadiusM), tauShell, static_cast<float>(0.12 * s.nebulaBright), shellKernel));
    prog_.set("uPwn", glm::vec4(static_cast<float>(Rp), tauPwn, static_cast<float>(0.45 * s.pulsarBright), pwnKernel));
    prog_.set("uFilThreshold", kFilThreshold);
    prog_.set("uWispReach", wispReach);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufPos_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufVel_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufAux_);
    glDispatchCompute(static_cast<GLuint>((count_ + 255) / 256), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    for (GLuint b : {0u, 1u, 2u}) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0);
    glUseProgram(0);
    cellScale_ = static_cast<float>(std::max(std::max(1.3 * s.envelopeRadiusM, s.shellRadiusM), 1e-9));
}

GasParticlesRef SnEvent::particles() const {
    GasParticlesRef ref;
    if (!ready_ || !active_ || !state_.breakout) return ref;
    ref.pos = bufPos_;
    ref.vel = bufVel_;
    ref.aux = bufAux_;
    ref.params = bufParams_;
    ref.paramsOffset = 0;
    ref.paramsSize = sizeof(ParamsBlock);
    ref.count = count_;
    ref.starRadius = cellScale_;
    ref.law = 1;
    ref.origin = state_.site;
    ref.noiseMod = 0.5f;   // soft mottling: the fingers and filaments carry their own structure
    ref.wantStats = false;
    return ref;
}

void SnEvent::appendBodies(std::vector<BodyDesc>& bodies) const {
    if (!active_) return;
    const State& s = state_;
    if (s.photoOpacity <= 0.0 || s.photoRadiusM <= 0.0) return;
    // The star, then the flash and the expanding envelope: one sphere whose radius, temperature and
    // brightness follow the model. The star is an ordinary convecting supergiant; from the breakout on it
    // is an exploding photosphere (big boiling plumes, evolving on the event's clock) that turns
    // transparent once the ejecta thin out, showing the remnant inside.
    BodyDesc b;
    b.kind = BodyKind::Star;
    b.pos = s.site;
    b.radius = s.photoRadiusM;
    b.teff = static_cast<float>(s.photoTempK);
    b.seed = 31;
    b.collidable = false;
    if (!s.breakout) {
        b.name = model_.params().starTeffK < 6000.0 ? "Red supergiant" : "Blue supergiant";
        b.lightIntensity = 1.0f;
        b.atmosphere = 4.0f;   // a few giant convection cells per radius
        b.info = "A star near the end of its life";
    } else {
        const double flash = model_.flashSeconds();
        b.name = "Supernova";
        b.lightIntensity = static_cast<float>(0.7 * s.photoBright);
        b.atmosphere = 3.0f;
        b.turbulence = static_cast<float>(std::clamp(0.45 + 0.2 * std::log10(1.0 + s.tauS / flash), 0.45, 1.0));
        b.phase = static_cast<float>(1.2 * std::log(1.0 + s.tauS / (0.3 * flash)));
        b.opacity = static_cast<float>(s.photoOpacity);
        b.info = "The envelope, flying apart";
    }
    bodies.push_back(b);
}

}  // namespace bhs::sn
