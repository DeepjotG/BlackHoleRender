#pragma once
// Test and diagnostics hooks of TdeSim that are not part of the public contract (TdeSim.h). Used by
// the headless GPU harness; the application does not need them.
#include <glad/gl.h>
#include <glm/glm.hpp>

#define TDE_DEBUG_V2 1   // the fields marked (v2) exist

namespace bhs::tde {

struct TdeDebugOptions {
    bool blackHoleGravity = true;     // false: no black hole; the star is placed at rest at the origin
                                      // and released at once (hydrostatic equilibrium checks)
    int maxHydroStepsOverride = 0;    // > 0: fixed hydro-step budget per frame instead of the cost model
    bool measureGpuTime = true;       // timestamp queries around the simulation passes
    float pressureScale = 1.0f;       // physics switches for diagnosis (1 = normal)
    float viscosityScale = 1.0f;      // both relaxation terms (compression viscosity and noise damping)
    float compressionViscosityScale = 1.0f;   // (v2) the compression term alone
    float noiseDampingScale = 1.0f;           // (v2) the noise-damping term alone
    bool selfGravity = true;
    bool profilePasses = false;       // per-pass GPU timestamps of the first hydro step (synchronous)
    int forceRemnantOp = 0;           // tests: 8 thaws, 9 removes a frozen remnant at the next step
    bool selfGravityFade = true;      // (v2) self-gravity fades out while almost no gas is bound to a core
    bool unweightedVolumes = false;   // (v2) volumes without the ownership weights: every grid shows all of
                                      // its gas (seam checks; overlapping volumes then double count)
};

// Passes timed by profilePasses (TdeDebugInfo::passMs). "restrict" includes grid 0's restriction into the
// black-hole nest (tde_g0nest.comp), "deposit" is the particle deposit alone.
inline constexpr const char* kTdePassNames[] = {"core trajectory", "update", "clear", "deposit", "restrict",
                                                "cells", "core find", "volumes"};
inline constexpr int kTdePassCount = 8;

// Read by spawn() and step(); change before spawning.
TdeDebugOptions& debugOptions();

struct TdeDebugInfo {
    double gpuMs = 0.0;               // GPU time of the frame two steps back (when measured)
    int hydroSteps = 0;               // hydro steps in the last step() call
    double hydroDt = 0.0;             // their length (M)
    double hydroDtLimit = 0.0;        // current limit from the time-step histogram
    int maxHydroSteps = 0;            // budget per frame
    int cappedParticles = 0;          // particles that hit the substep cap (two frames back)
    int nonFiniteParticles = 0;       // removed because their state became non-finite (cumulative)
    int occupiedCells[4] = {0, 0, 0, 0};
    int liveBlocks = 0;               // 8^3-cell blocks processed per grid pass (all grids)
    glm::vec4 grid[4];                // xyz centre, w half size (0 = inactive) of the last deposit
    glm::dvec3 corePos{0.0};
    bool coreValid = false;
    double coreParticles = 0.0;       // particles' worth of mass near the density peak of grid 0
    double g0OuterFraction = 0.0;     // mass in the outer eighth of grid 0 / mass in grid 0
    GLuint particleBuffers[3] = {0, 0, 0};   // pos (xyz, state), vel (xyz, u), aux (A0, heat, u, rho)
    int particleCount = 0;
    double passMs[8] = {};            // with profilePasses: GPU time of each pass (kTdePassNames)
    // (v2) core, remnant and self-gravity state (two frames back)
    bool coreMode = false;            // a dense, round, stable core: debris grid policy and freezing
    bool remnant = false;             // gas bound to the core counts with the core's orbital energy
    bool frozen = false;              // the remnant moves rigidly
    double boundMass = 0.0;           // M_b: fraction of the star bound to the core (any distance)
    double boundNearCore = 0.0;       // fraction of the star within d_c of the core and bound to it
    double elongation = 0.0;          // elongation weight of the gas near the core (0 round, 1 filament)
    double peakDensity = 0.0;         // densest cell of the debris grid / the star's mean density
    double selfGravityWeight = 0.0;   // weight of the self-gravity in the current step
    // (v2) cost model
    double predictedStepMs = 0.0;     // one hydro step
    double predictedFrameMs = 0.0;    // the last frame: its hydro steps and the volumes
    double substepsPerStep = 0.0;     // inputs: gravity substeps and global cell updates of the deposit in
    double writesPerStep = 0.0;       // the last step of the frame two frames back
    int readbackSteps = 0;            // hydro steps of the frame that gpuMs and the counters above belong to
    double readbackActive = 0.0;      // its active particles and live blocks
    double readbackBlocks = 0.0;
    double profSubsteps = 0.0;        // with profilePasses: the same counters of the profiled (first) step,
    double profWrites = 0.0;          // read synchronously, and its active particles and live blocks
    double profActive = 0.0;
    double profBlocks = 0.0;
};

// Diagnostics of the most recent step() of any TdeSim.
const TdeDebugInfo& debugInfo();

}  // namespace bhs::tde
