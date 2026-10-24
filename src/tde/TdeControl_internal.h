#pragma once
// Decision logic of TdeSim that does not touch OpenGL, kept here so tests/tde_tests.cpp can check it on
// the CPU: the frame plan of the GPU cost model and the core state machine (remnant classification of
// the bound fraction, self-gravity switch). Everything is deterministic: inputs are simulation state and
// counters read back from the GPU, never timer measurements.
#include <algorithm>
#include <cmath>

namespace bhs::tde::control {

// ------------------------------------------------------------------------------------------------ cost
// One hydro step of length h is predicted to cost c0 + c1 h (ms): a fixed part, the particles (update and
// deposit), the global cell updates of the deposit, the live blocks (clear, restriction, cells) and one
// gravity substep per updated particle, plus the remaining substeps, which grow with the step length
// (scaled from the last measured step). The volume pass costs volumeFixedMs + volumePerBlockMs per live
// block.
struct CostModel {   // defaults: the calibration of TdeSim.cpp (kCost)
    double budgetMs = 2.4;
    double stepFixedMs = 0.19;
    double perParticleMs = 4.8e-6;
    double perSubstepMs = 1.1e-7;
    double perWriteMs = 0.0;
    double perBlockMs = 5.0e-4;
    double volumeFixedMs = 0.05;
    double volumePerBlockMs = 1.8e-4;
    int maxSteps = 8;                // hydro steps per frame
    double minStepFraction = 0.125;  // shortest step it makes, as a fraction of the hydro step limit
    int maxSubsteps = 48;            // gravity substeps per particle and step (the update's cap)
};

// Work of the last step that was read back (the frame two back).
struct WorkCounters {
    bool valid = false;        // false until the first frame after release has been read back
    double particles = 0.0;    // active particles, a frozen remnant included (they deposit)
    double updated = 0.0;      // particles that run the gravity substeps (active, not frozen)
    double substeps = 0.0;     // gravity substeps of the measured step
    double writes = 0.0;       // global cell updates of its deposit
    double stepLength = 0.0;   // its length (M)
    double blocks = 0.0;       // live blocks
};

inline double finiteOr0(double x) { return std::isfinite(x) ? x : 0.0; }

// c0: a step of zero length (one substep per particle).
inline double stepBaseMs(const CostModel& m, const WorkCounters& w) {
    const double particles = std::max(finiteOr0(w.particles), 1.0);
    const double updated = std::max(finiteOr0(w.updated), 1.0);
    const double writes = w.valid ? std::max(finiteOr0(w.writes), 0.0) : 8.0 * particles;
    const double blocks = std::max(finiteOr0(w.blocks), 64.0);
    return m.stepFixedMs + m.perParticleMs * particles + m.perWriteMs * writes + m.perBlockMs * blocks + m.perSubstepMs * updated;
}

// c1 (ms per M of step length).
inline double stepSlopeMs(const CostModel& m, const WorkCounters& w) {
    if (!w.valid || !(w.stepLength > 0.0) || !std::isfinite(w.stepLength)) return 0.0;
    const double updated = std::max(finiteOr0(w.updated), 1.0);
    return m.perSubstepMs * std::max(finiteOr0(w.substeps) - updated, 0.0) / w.stepLength;
}

inline double stepCostMs(const CostModel& m, const WorkCounters& w, double h) {
    const double updated = std::max(finiteOr0(w.updated), 1.0);
    const double capped = m.perSubstepMs * (m.maxSubsteps - 1) * updated;   // the update's per-particle cap
    // Before the first counters: assume 24 substeps per particle.
    const double extra = w.valid ? stepSlopeMs(m, w) * std::max(finiteOr0(h), 0.0) : m.perSubstepMs * 23.0 * updated;
    return stepBaseMs(m, w) + std::min(extra, capped);
}

inline double volumeCostMs(const CostModel& m, const WorkCounters& w) {
    return m.volumeFixedMs + m.volumePerBlockMs * std::max(finiteOr0(w.blocks), 64.0);
}

// Steps for a frame with a hydro step limit dtH: as many full steps as fit the budget (at least one); when
// even one full step does not fit (many gravity substeps per step), a single shorter step that does, but
// not shorter than minStepFraction dtH (and not shorter at all when the fixed part alone exceeds it).
struct FramePlan {
    int maxSteps = 1;
    double maxStep = 0.0;   // M
};

inline FramePlan planFrame(const CostModel& m, const WorkCounters& w, double dtH) {
    FramePlan plan{1, dtH};
    const double room = m.budgetMs - volumeCostMs(m, w);
    const double full = stepCostMs(m, w, dtH);
    if (full <= room) {
        plan.maxSteps = std::clamp(static_cast<int>(room / full), 1, std::max(m.maxSteps, 1));
        return plan;
    }
    const double c0 = stepBaseMs(m, w), c1 = stepSlopeMs(m, w);
    if (c1 > 0.0 && room > c0) plan.maxStep = std::clamp((room - c0) / c1, m.minStepFraction * dtH, dtH);
    return plan;
}

// Splits a requested advance dt into equal steps within the plan. `limited`: less than dt is advanced.
struct FrameSteps {
    int steps = 0;
    double h = 0.0;
    double advance = 0.0;
    bool limited = false;
};

inline FrameSteps frameSteps(const FramePlan& plan, double dt) {
    FrameSteps s;
    const double hMax = std::max(plan.maxStep, 1e-9);
    s.advance = std::min(dt, plan.maxSteps * hMax);
    s.limited = s.advance < dt * (1.0 - 1e-9);
    s.steps = s.advance > 0.0 ? std::clamp(static_cast<int>(std::ceil(s.advance / hMax - 1e-9)), 1, plan.maxSteps) : 0;
    s.h = s.steps > 0 ? s.advance / s.steps : 0.0;
    return s;
}

// ------------------------------------------------------------------------------------------------ core
// What the readback of one frame tells about the gas around the core (times in M).
struct CoreObservation {
    double t = 0.0;            // simulation time at the end of that frame
    double frameDt = 0.0;      // time that frame advanced
    bool g0Active = true;      // the debris grid (and its core tracker) is running
    bool coreMode = false;     // a dense, round, stable core (TdeSim's debris-grid policy)
    bool frozen = false;       // a remnant moving rigidly
    bool coreValid = false;    // the core tracker has a position and velocity
    bool outbound = false;     // ... and the core moves away from the hole
    double boundMass = 0.0;    // M_b: fraction of the star bound to the core, any distance
};

struct CoreParams {
    double tStar = 1.0;              // dynamical time of the star sqrt(R^3 / q)
    double remnantDelay = 2.0;       // remnant classification from this many tStar after pericentre ...
    double selfGravityBound = 0.01;  // self-gravity fades when M_b is below this, without a core, ...
    double selfGravityOffDelay = 2.0;   // ... for this many tStar,
    double selfGravityFade = 2.0;       // ... over this many tStar
    bool fadeEnabled = true;
    bool blackHole = true;           // without a black hole (hydrostatic tests) nothing switches
};

// Remnant classification: before pericentre the intact star is classified particle by particle (the
// frozen-in split it would have if torn apart now); a core that is still there remnantDelay dynamical
// times after pericentre (the first time the core moves away from the hole) and has been dense, round and
// stable (coreMode) for one dynamical time, or a frozen remnant, is a remnant moving as one body: the gas
// bound to it counts with its orbital energy.
// Self-gravity (a monopole around the core) is meant for gas bound to a core. Once there has been no core
// and less than selfGravityBound of the star bound to it for selfGravityOffDelay dynamical times (the
// stream of a full disruption), its weight fades to 0 over selfGravityFade dynamical times; it comes back
// at the same rate when a core or bound gas returns.
class CoreState {
public:
    void reset() {
        tOutbound_ = coreSince_ = lowSince_ = -1.0;
        remnant_ = false;
        weight_ = 1.0;
    }

    void observe(const CoreObservation& o, const CoreParams& p) {
        if (!p.blackHole) {
            remnant_ = false;
            weight_ = 1.0;
            return;
        }
        const double tStar = std::max(p.tStar, 1e-300);
        if (tOutbound_ < 0.0 && o.coreValid && o.outbound) tOutbound_ = o.t;
        const bool core = o.g0Active && o.coreMode;
        if (!core) coreSince_ = -1.0;
        else if (coreSince_ < 0.0) coreSince_ = o.t;
        remnant_ = o.frozen ||
                   (core && tOutbound_ >= 0.0 && o.t >= tOutbound_ + p.remnantDelay * tStar && o.t - coreSince_ >= tStar);
        const bool low = !o.frozen && !core && !(o.boundMass >= p.selfGravityBound);
        if (!low) lowSince_ = -1.0;
        else if (lowSince_ < 0.0) lowSince_ = o.t;
        const bool off = p.fadeEnabled && low && o.t - lowSince_ >= p.selfGravityOffDelay * tStar;
        const double change = std::max(finiteOr0(o.frameDt), 0.0) / (std::max(p.selfGravityFade, 1e-6) * tStar);
        weight_ = std::clamp(weight_ + (off ? -change : change), 0.0, 1.0);
    }

    bool remnant() const { return remnant_; }
    double selfGravityWeight() const { return weight_; }
    double outboundTime() const { return tOutbound_; }

private:
    double tOutbound_ = -1.0;   // first time the core was seen moving away from the hole (after pericentre)
    double coreSince_ = -1.0;   // since when coreMode has held
    double lowSince_ = -1.0;    // since when neither a core nor enough bound gas
    bool remnant_ = false;
    double weight_ = 1.0;
};

}  // namespace bhs::tde::control
