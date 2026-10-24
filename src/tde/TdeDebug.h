#pragma once
// Test and diagnostics hooks of TdeSim that are not part of the public contract (TdeSim.h). Used by
// the headless GPU harness; the application does not need them.
#include <glad/gl.h>
#include <glm/glm.hpp>

namespace bhs::tde {

struct TdeDebugOptions {
    bool blackHoleGravity = true;     // false: no black hole; the star is placed at rest at the origin
                                      // and released at once (hydrostatic equilibrium checks)
    int maxHydroStepsOverride = 0;    // > 0: fixed hydro-step budget per frame instead of the cost model
    bool measureGpuTime = true;       // timestamp queries around the simulation passes
    float pressureScale = 1.0f;       // physics switches for diagnosis (1 = normal)
    float viscosityScale = 1.0f;
    bool selfGravity = true;
    bool profilePasses = false;       // per-pass GPU timestamps of the first hydro step (synchronous)
    int forceRemnantOp = 0;           // tests: 8 thaws, 9 removes a frozen remnant at the next step
};

// Passes timed by profilePasses (TdeDebugInfo::passMs).
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
};

// Diagnostics of the most recent step() of any TdeSim.
const TdeDebugInfo& debugInfo();

}  // namespace bhs::tde
