// Star disruption simulation (contract in TdeSim.h). Overview:
//  * Before release the star is a rigid body on its parabolic orbit, integrated here in double
//    precision with the force law of TdePhysics.h; swallowed-whole stars stay intact until the centre
//    crosses r = 2.05 M.
//  * At release the particles (a stratified sample of the Lane-Emden polytrope) are placed on the GPU
//    and their internal energy is set up so that the grid pressure balances the monopole self-gravity
//    shell by shell (tde_init.comp, with three force-measured corrections).
//  * Each frame advances one or more hydro steps (length from a histogram of per-particle step
//    requirements read back two frames late; count, or a shorter single step, from a deterministic GPU
//    cost model fed by work counters read back with it, TdeControl_internal.h). A hydro step is:
//    core trajectory -> particle update (pressure, relaxation/viscosity, PdV, cooling, sub-cycled
//    gravity, accretion/escape) -> deposit (integer atomics) -> restriction through the black-hole
//    nest -> block dilation -> decoding into fields -> core centroid and monopole table.
//  * Grids: grid 0 follows the star / remnant / densest debris; grids 1-3 are a black-hole-centred
//    nest with power-of-two size ratios. All are 64^3 and processed in 8^3 blocks that hold gas.
//  * Self-gravity is a monopole around the core while gas is bound to one; it fades out in the stream
//    of a full disruption. The bound fraction counts the gas of a surviving remnant with the remnant's
//    orbital energy (control::CoreState).
//  * A surviving core that leaves the tidal zone is frozen (moves rigidly, still deposited) so it no
//    longer dictates the step; gas falling into it joins it; it thaws if it comes back.
//  * After the last step the four grids are written to RGBA16F volumes through tde_visual.glsl, the
//    coarsest nest level first (it also provides the particle counts of the noise fade for all grids).
#include "tde/TdeSim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "Log.h"
#include "Shader.h"
#include "Units.h"
#include "tde/TdeControl_internal.h"
#include "tde/TdeDebug.h"
#include "tde/TdePhysics.h"

namespace bhs::tde {

namespace {

// Must match shaders/tde_common.glsl.
constexpr int kRes = 64;
constexpr int kCells = kRes * kRes * kRes;
constexpr int kGrids = TdeSim::kMaxVolumes;
constexpr int kWords = 16;
constexpr int kBins = 256;
constexpr int kDtBins = 128;
constexpr int kTraj = 64;
constexpr int kStatWords = 64 + kDtBins;
constexpr int kStatCumulative = 16;
constexpr int S_ACCRETED = 0, S_ESCAPED = 1, S_NAN = 2;
constexpr int S_ACTIVE = 16, S_BOUND = 17, S_DISS = 18, S_CAPPED = 20, S_OCC = 21, S_G0_MASS = 25, S_G0_OUTER = 26, S_LIVE = 27,
              S_FROZEN = 28, S_SUBSTEPS = 29, S_GWRITES = 30;
constexpr int S_DTHIST = 64;
constexpr GLuint kUboBinding = 3;
constexpr GLuint kSsboBindings = 11;
constexpr int kBlocksPerGrid = (kRes / 8) * (kRes / 8) * (kRes / 8);
// Word offsets in the Mono block (tde_common.glsl).
constexpr int kMonoCore = 2 * (kBins + 2);
constexpr int kMonoRho = kMonoCore + 32;
constexpr int kMonoAr = kMonoRho + 2 * kBins + kBins + kBins + 1;
constexpr int kMonoWords = kMonoAr + 2 * kBins;

// ---------------------------------------------------------------- tunables
constexpr double kEta = 0.03;              // substep = eta * local dynamical time (TdePhysics.h)
constexpr int kMaxSubsteps = 48;           // per particle and hydro step
constexpr double kViscosity = 2.0;         // sticky viscosity: relaxation within half a cell of relative motion
constexpr double kNoiseDamping = 1.5;      // relaxation at kappa c_s / cell: damps the particle-grid noise
constexpr double kCoolingOrbits = 1.0;     // cooling time of the heat above the initial adiabat, in local orbits
constexpr double kAccelCap = 2.0;          // pressure acceleration <= kAccelCap * cell / dt^2
constexpr double kCfl = 0.4;               // hydro step <= kCfl * cell / c_s
constexpr double kCAcc = 0.5;              // hydro step <= kCAcc * sqrt(cell / |a_pressure|)
constexpr double kCOrb = 0.05;             // hydro step <= kCOrb * local orbital period (gas in grids)
constexpr double kDtTailFraction = 0.005;  // fraction of particles allowed to violate the step limit
// GPU cost model (RTX 3060 Laptop at its sustained clocks, fitted to 20k whole-frame GPU times of named
// and extreme events at 131k and 262k particles; TdeControl_internal.h). One hydro step costs
//     stepFixedMs + perParticleMs * particles + perSubstepMs * gravity substeps + perBlockMs * live blocks,
// with the work counters of the last hydro step two frames back (the update pass grows with the
// substeps, which grow with the step length), and the volume pass volumeFixedMs + volumePerBlockMs per
// live block. The deposit's global updates are counted too but left out (perWriteMs 0): they track the
// particle count. The fit is slightly conservative (measured / predicted: p99 <= 1.14 for most events,
// 1.24 for the worst, a bloated 0.08 Msun star at 1e10 Msun), and the budget leaves room for that, so the
// frame stays within 3 ms. The steps of a frame are chosen from these (deterministic, unlike timer
// feedback): as many full steps as fit, or one shorter step when the gravity substeps make even one too
// costly.
constexpr control::CostModel kCost{
    /*budgetMs*/ 2.4, /*stepFixedMs*/ 0.19, /*perParticleMs*/ 4.8e-6, /*perSubstepMs*/ 1.1e-7, /*perWriteMs*/ 0.0,
    /*perBlockMs*/ 5.0e-4, /*volumeFixedMs*/ 0.05, /*volumePerBlockMs*/ 1.8e-4, /*maxSteps*/ 8,
    /*minStepFraction*/ 0.125, /*maxSubsteps*/ kMaxSubsteps};
constexpr double kG0InitialHalf = 1.4;     // debris grid half size at release, in stellar radii
constexpr double kCoreBound = 0.03;        // a core survives while this fraction of the star is bound to it
// Remnant classification of the bound fraction and the self-gravity switch (TdeControl_internal.h), in
// dynamical times of the star: a core still there 2 t* after pericentre that has been dense, round and
// stable for t* is a remnant; self-gravity fades out over 2 t* once, for 2 t*, there has been no such core
// and less than 1% of the star bound to the core. The threshold is low and the delay long so that a small
// surviving remnant, whose bound mass reads low while its tails confuse the core tracker, keeps its gravity.
constexpr double kRemnantDelay = 2.0;
constexpr double kSelfGravityBound = 0.01;
constexpr double kSelfGravityOffDelay = 2.0;
constexpr double kSelfGravityFade = 2.0;

// std140 mirror of the TdeParams block.
struct alignas(16) ParamsUbo {
    glm::vec4 gridDep[kGrids];
    glm::vec4 gridFld[kGrids];
    glm::vec4 ramp;
    glm::vec4 event;
    glm::vec4 step;
    glm::vec4 mono;
    glm::vec4 scale;
    glm::vec4 phys;
    glm::vec4 limit;
    glm::vec4 vis;
    glm::vec4 poly;
    glm::vec4 extra;
    glm::vec4 frozen;
    glm::vec4 frozenVel;
    glm::ivec4 info;
};
static_assert(sizeof(ParamsUbo) == 21 * 16, "std140 layout");

struct ReadbackData {
    uint32_t words[kStatWords];
    float core[12];
};

uint32_t spreadBits(uint32_t v) {
    v &= 0x3ffu;
    v = (v | (v << 16)) & 0x030000ffu;
    v = (v | (v << 8)) & 0x0300f00fu;
    v = (v | (v << 4)) & 0x030c30c3u;
    v = (v | (v << 2)) & 0x09249249u;
    return v;
}

double cellOf(const glm::dvec4& g) { return 2.0 * g.w / kRes; }

bool insideBox(const glm::dvec4& g, const glm::dvec3& p, double margin) {
    if (g.w <= 0.0) return false;
    const glm::dvec3 d = glm::abs(p - glm::dvec3(g));
    return std::max({d.x, d.y, d.z}) <= g.w - margin;
}

}  // namespace

TdeDebugOptions& debugOptions() {
    static TdeDebugOptions options;
    return options;
}

namespace {
TdeDebugInfo g_debugInfo;
}
const TdeDebugInfo& debugInfo() { return g_debugInfo; }

// ================================================================================================
struct TdeSim::Impl {
    // ---------------------------------------------------------------- GL resources
    bool initialized = false;
    int maxParticles = 0;
    ShaderProgram progUpdate, progDeposit, progRestrict, progCells, progCore, progInit, progVolume, progBlocks, progG0Nest;
    GLuint bufPos = 0, bufVel = 0, bufAux = 0, bufAcc = 0, bufFA = 0, bufFB = 0, bufMono = 0, bufCore = 0, bufStats = 0, bufBlk = 0, bufFC = 0;
    GLuint bufUbo = 0;
    size_t uboStride = 0;
    int uboSlot = 0;
    static constexpr int kUboSlots = 64;
    static constexpr int kRing = 3;
    GLuint bufReadback[kRing] = {};
    GLsync fence[kRing] = {};
    GLuint query[2 * kRing] = {};   // timestamp pairs (a GL_TIME_ELAPSED query could nest with the renderer's)
    bool ringValid[kRing] = {};
    bool ringHadSteps[kRing] = {};
    bool ringTimed[kRing] = {};
    double ringTime[kRing] = {};
    double ringDt[kRing] = {};
    int ringEpoch[kRing] = {};      // remnantEpoch when the frame was recorded
    int ringSteps[kRing] = {};
    GLuint textures[kGrids] = {};
    GLuint texFadeRef = 0;          // particle count of the coarsest nest level around each texel (noise fade)

    // ---------------------------------------------------------------- event
    bool spawned = false, released = false, finished = false, noBh = false;
    StarParams star;
    OrbitParams orbit;
    double bhMassSolar = 0.0;
    phys::EventSetup ev;
    phys::LaneEmden le;
    phys::PolytropeModel poly;
    int count = 0;
    double time = 0.0;
    glm::dvec3 comX{0.0}, comV{0.0};
    double uStar = 0.0, tStar = 0.0, tMin = 0.0, rhoStar = 0.0;
    double energyScale = 0.0, heatScale = 0.0, dissUnit = 0.0;
    uint64_t gpuFrame = 0;

    // ---------------------------------------------------------------- grids and core
    glm::dvec4 geomFld[kGrids]{}, geomDep[kGrids]{};
    double nestL1 = 0.0;
    int nestRatio = 2;
    bool g0Active = false;
    double g0Half = 0.0;
    int g0Starved = 0;
    double tRelease = 0.0;
    bool coreMode = false;     // a dense core survives
    static constexpr int kHistory = 64;
    std::array<std::pair<double, double>, kHistory> coreHistory{};   // (time, particles near the core)
    int historyHead = 0;
    // Frozen remnant (tde_init.comp modes 7-9): a surviving core far from the hole moves rigidly on its
    // orbit (integrated here in double precision) instead of being resolved hydrodynamically.
    bool frozen = false;
    glm::dvec3 frzX{0.0}, frzV{0.0}, frzXDep{0.0};
    double frzT = 0.0;
    double frzRadius = 0.0;
    int frozenCount = 0;
    int pendingFrozenOp = 0;   // 7 freeze, 8 thaw, 9 remove as escaped
    double freezeRetryTime = 0.0;
    // Every freeze, thaw or removal starts a new epoch: the frame counters of frames recorded before it
    // count different particles as active or frozen, so readback() does not use them for those counts.
    int remnantEpoch = 0;
    double dtAtFreeze = 0.0;   // hydro step limit when the remnant froze (its gas needs it again at a thaw)
    // Remnant classification and self-gravity (readback()).
    control::CoreState coreState;   // remnant flag (uInfo.w) and self-gravity weight (uExtra.z)
    double boundMass = 1.0;         // M_b, fraction of the star bound to the core (cInfo.w)
    // Cost model inputs: work counters per hydro step (last step of the frame two frames back).
    double substepsPerStep = 0.0, writesPerStep = 0.0;
    double hMeasured = 0.0;   // length of the step those counters belong to
    bool haveCounters = false;
    glm::dvec3 coreX{0.0}, coreV{0.0};
    double coreT = 0.0;
    bool coreValid = false;
    double coreParticles = 0.0;
    int occ[kGrids] = {};
    int liveBlocks = 0;
    int sinceActivation[kGrids] = {};

    // ---------------------------------------------------------------- control and output
    double dtHydroLimit = 0.0;
    TdeStats stats;
    std::array<GasVolume, kGrids> vols{};
    int volCount = 0;
    int lastSteps = 0;
    double lastDt = 0.0;

    // ============================================================ init / shutdown
    bool init(int maxP) {
        shutdown();
        maxParticles = std::clamp(maxP, 1024, 1 << 20);
        bool ok = progUpdate.loadCompute("tde_update.comp");
        ok = progDeposit.loadCompute("tde_deposit.comp") && ok;
        ok = progRestrict.loadCompute("tde_restrict.comp") && ok;
        ok = progCells.loadCompute("tde_cells.comp") && ok;
        ok = progCore.loadCompute("tde_core.comp") && ok;
        ok = progInit.loadCompute("tde_init.comp") && ok;
        ok = progVolume.loadCompute("tde_volume.comp") && ok;
        ok = progBlocks.loadCompute("tde_blocks.comp") && ok;
        ok = progG0Nest.loadCompute("tde_g0nest.comp") && ok;
        if (!ok) {
            logError("star disruption shaders failed to compile; disruptions are disabled");
            destroyPrograms();
            return false;
        }
        auto makeBuffer = [](GLuint& b, size_t bytes) {
            glCreateBuffers(1, &b);
            glNamedBufferStorage(b, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
        };
        const size_t np = static_cast<size_t>(maxParticles);
        makeBuffer(bufPos, np * 16);
        makeBuffer(bufVel, np * 16);
        makeBuffer(bufAux, np * 16);
        makeBuffer(bufAcc, static_cast<size_t>(kGrids) * kWords * kCells * 4);
        makeBuffer(bufFA, static_cast<size_t>(kGrids) * kCells * 16);
        makeBuffer(bufFB, static_cast<size_t>(kGrids) * kCells * 16);
        makeBuffer(bufFC, static_cast<size_t>(kGrids) * kCells * 4);
        // Mono block of tde_common.glsl: mHist, mCum (NBINS + 2 each), mCore (32), mRho (2 NBINS),
        // mCnt (NBINS), mP (NBINS + 1), mAr (2 NBINS).
        makeBuffer(bufMono, static_cast<size_t>(kMonoWords) * 4);
        makeBuffer(bufCore, (5 + kTraj + 1) * 16);
        makeBuffer(bufStats, kStatWords * 4);
        makeBuffer(bufBlk, static_cast<size_t>(kGrids) * kBlocksPerGrid * 4);
        GLint align = 256;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        align = std::max(align, 16);
        uboStride = (sizeof(ParamsUbo) + static_cast<size_t>(align) - 1) / static_cast<size_t>(align) * static_cast<size_t>(align);
        makeBuffer(bufUbo, uboStride * kUboSlots);
        for (int i = 0; i < kRing; ++i) {
            // Read back on the CPU: keep it in client memory (no migration on every read).
            glCreateBuffers(1, &bufReadback[i]);
            glNamedBufferStorage(bufReadback[i], sizeof(ReadbackData), nullptr, GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
        }
        glGenQueries(2 * kRing, query);
        const GLuint zero = 0;
        for (GLuint b : {bufPos, bufVel, bufAux, bufAcc, bufFA, bufFB, bufMono, bufCore, bufStats, bufBlk, bufFC})
            glClearNamedBufferData(b, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glCreateTextures(GL_TEXTURE_3D, kGrids, textures);
        glCreateTextures(GL_TEXTURE_3D, 1, &texFadeRef);
        for (GLuint t : textures) glTextureStorage3D(t, 1, GL_RGBA16F, kRes, kRes, kRes);
        glTextureStorage3D(texFadeRef, 1, GL_R16F, kRes, kRes, kRes);
        for (int i = 0; i <= kGrids; ++i) {
            const GLuint t = i < kGrids ? textures[i] : texFadeRef;
            glTextureParameteri(t, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(t, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(t, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(t, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteri(t, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glClearTexImage(t, 0, i < kGrids ? GL_RGBA : GL_RED, GL_FLOAT, nullptr);
        }
        initialized = true;
        logInfo("star disruption simulation ready ({} particles max, {}^3 grids)", maxParticles, kRes);
        return true;
    }

    void destroyPrograms() {
        for (ShaderProgram* p : {&progUpdate, &progDeposit, &progRestrict, &progCells, &progCore, &progInit, &progVolume, &progBlocks, &progG0Nest})
            p->destroy();
    }

    void shutdown() {
        if (!initialized && !progUpdate.valid()) return;
        destroyPrograms();
        for (GLuint* b : {&bufPos, &bufVel, &bufAux, &bufAcc, &bufFA, &bufFB, &bufMono, &bufCore, &bufStats, &bufBlk, &bufFC, &bufUbo}) {
            if (*b) glDeleteBuffers(1, b);
            *b = 0;
        }
        for (int i = 0; i < kRing; ++i) {
            if (bufReadback[i]) glDeleteBuffers(1, &bufReadback[i]);
            bufReadback[i] = 0;
            if (fence[i]) glDeleteSync(fence[i]);
            fence[i] = nullptr;
            ringValid[i] = false;
        }
        if (query[0]) glDeleteQueries(2 * kRing, query);
        std::fill(std::begin(query), std::end(query), 0u);
        if (profQ[0]) glDeleteQueries(kTdePassCount + 2, profQ);
        std::fill(std::begin(profQ), std::end(profQ), 0u);
        if (textures[0]) glDeleteTextures(kGrids, textures);
        std::fill(std::begin(textures), std::end(textures), 0u);
        if (texFadeRef) glDeleteTextures(1, &texFadeRef);
        texFadeRef = 0;
        initialized = false;
        spawned = false;
        volCount = 0;
    }

    // ============================================================ spawn / clear
    void clear() {
        spawned = released = finished = false;
        frozen = false;
        frozenCount = 0;
        pendingFrozenOp = 0;
        coreMode = false;
        freezeRetryTime = 0.0;
        remnantEpoch = 0;
        dtAtFreeze = 0.0;
        coreState.reset();
        boundMass = 1.0;
        substepsPerStep = writesPerStep = 0.0;
        hMeasured = 0.0;
        haveCounters = false;
        coreHistory.fill({0.0, 0.0});
        historyHead = 0;
        stats = TdeStats{};
        volCount = 0;
        g0Active = false;
        coreValid = false;
        for (int i = 0; i < kRing; ++i) {
            if (fence[i]) glDeleteSync(fence[i]);
            fence[i] = nullptr;
            ringValid[i] = false;
        }
        for (auto& g : geomFld) g = glm::dvec4(0.0);
        for (auto& g : geomDep) g = glm::dvec4(0.0);
    }

    void spawn(const StarParams& s, const OrbitParams& o, double bhMass, int particles) {
        clear();
        star = s;
        orbit = o;
        bhMassSolar = bhMass;
        noBh = !debugOptions().blackHoleGravity;
        phys::EventInputs in;
        in.starMassSolar = s.massSolar;
        in.starRadiusSolar = s.radiusSolar;
        in.bhMassSolar = bhMass;
        in.beta = o.beta;
        in.inclinationDeg = o.inclinationDeg;
        in.longitudeDeg = o.longitudeDeg;
        in.retrograde = o.retrograde;
        in.startDistanceRt = o.startDistanceRt;
        ev = phys::setupEvent(in);
        if (!std::isfinite(star.teff) || star.teff <= 0.0) star.teff = 5772.0;
        star.teff = std::clamp(star.teff, 1500.0, 60000.0);
        if (noBh) {
            ev.swallowed = false;
            ev.x0 = glm::dvec3(0.0);
            ev.v0 = glm::dvec3(0.0);
        }
        stats.tidalRadiusM = ev.rt;
        stats.pericenterM = ev.rp;
        stats.starRadiusM = ev.R;
        stats.swallowedWhole = ev.swallowed;
        comX = ev.x0;
        comV = ev.v0;
        time = 0.0;
        spawned = true;
        uStar = ev.q / ev.R;
        tStar = std::sqrt(ev.R * ev.R * ev.R / ev.q);
        const double aMin = ev.rt * ev.rt / (2.0 * ev.R);
        tMin = 2.0 * phys::kPi * std::pow(aMin, 1.5);
        rhoStar = 3.0 / (4.0 * phys::kPi * ev.R * ev.R * ev.R);
        if (!initialized) {
            logWarn("star disruption: spawn() before a successful init(); only the intact star is simulated");
            return;
        }
        if (ev.swallowed) return;   // the intact star falls in; no particles needed

        count = std::clamp(particles, 1024, maxParticles);
        double n = star.polytropeN;
        if (!std::isfinite(n)) n = 3.0;
        le = phys::solveLaneEmden(std::clamp(n, 1.0, 4.0));
        poly = phys::makePolytrope(le, ev.q, ev.R);
        const std::vector<glm::dvec3> pts = phys::samplePolytrope(le, count, 0x7de5u);
        // Morton order keeps neighbouring particles together in memory (coherent atomics and gathers).
        std::vector<std::pair<uint32_t, int>> keys(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const glm::dvec3 p = (pts[static_cast<size_t>(i)] + 1.0) * 511.5;
            const uint32_t k = spreadBits(static_cast<uint32_t>(p.x)) | (spreadBits(static_cast<uint32_t>(p.y)) << 1) |
                               (spreadBits(static_cast<uint32_t>(p.z)) << 2);
            keys[static_cast<size_t>(i)] = {k, i};
        }
        std::sort(keys.begin(), keys.end());
        std::vector<glm::vec4> pos(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            pos[static_cast<size_t>(i)] = glm::vec4(glm::vec3(pts[static_cast<size_t>(keys[static_cast<size_t>(i)].second)] * ev.R), 0.0f);
        glNamedBufferSubData(bufPos, 0, static_cast<GLsizeiptr>(pos.size() * sizeof(glm::vec4)), pos.data());
        const GLuint zero = 0;
        glClearNamedBufferData(bufVel, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glClearNamedBufferData(bufAux, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glClearNamedBufferData(bufStats, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glClearNamedBufferData(bufAcc, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glClearNamedBufferData(bufBlk, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);

        // Fixed-point scales and grids.
        energyScale = std::min(1073741824.0 / (1e3 * uStar), 2e18);
        heatScale = std::min(1073741824.0 / (1e3 * uStar / tStar), 2e18);
        dissUnit = std::max(1e-3 * uStar, 1e-12);
        nestL1 = std::max(3.0 * ev.rp, 12.0);
        const double want = std::sqrt(std::max(2.4 * aMin / nestL1, 1.0));
        nestRatio = 1 << std::clamp(static_cast<int>(std::lround(std::log2(want))), 1, 3);   // 2, 4 or 8
        if (noBh) {
            nestL1 = 8.0 * ev.R;
            nestRatio = 4;
        }
        gpuFrame = 0;
        dtHydroLimit = 0.0;
        logInfo("star disruption: q={:.3g} R={:.4g} M r_t={:.4g} M r_p={:.4g} M, {} particles, nest {:.4g} M x{}",
                ev.q, ev.R, ev.rt, ev.rp, count, nestL1, nestRatio);
    }

    // ============================================================ helpers
    double lapse(const glm::dvec3& x, const glm::dvec3& v) const { return noBh ? 1.0 : phys::lapse(x, v); }

    // Ballistic (black hole only) prediction, used for the rigid star and the core tracker.
    void advanceBallistic(glm::dvec3& x, glm::dvec3& v, double dt, bool* accreted = nullptr) const {
        if (noBh || dt <= 0.0) return;
        const phys::Fate f = phys::advanceParticle(x, v, dt, phys::SubcycleConfig{0.004, 1 << 16});
        if (accreted) *accreted = f == phys::Fate::Accreted;
    }

    glm::dvec3 predictCore(double t) {
        if (frozen) {
            advanceFrozen(t);
            coreX = frzX;
            coreV = frzV;
            coreT = frzT;
            return coreX;
        }
        if (t > coreT) {
            advanceBallistic(coreX, coreV, t - coreT);
            coreT = t;
        }
        return coreX;
    }

    void advanceFrozen(double t) {
        if (t > frzT) {
            advanceBallistic(frzX, frzV, t - frzT);
            frzT = t;
        }
    }

    // The remnant's region: tidal forces there are below ~4% of those at the tidal radius.
    double quietRadius() const { return 3.0 * std::max(ev.rt, ev.rp); }

    // Cost model inputs (TdeControl_internal.h): the work of the last step read back.
    control::WorkCounters workCounters() const {
        control::WorkCounters w;
        w.valid = haveCounters;
        w.particles = stats.activeParticles > 0 ? stats.activeParticles : count;
        w.updated = std::max(w.particles - (frozen ? frozenCount : 0), 1.0);
        w.substeps = substepsPerStep;
        w.writes = writesPerStep;
        w.stepLength = hMeasured;
        w.blocks = liveBlocks;
        return w;
    }
    double stepCostMs(double h) const { return control::stepCostMs(kCost, workCounters(), h); }
    double volumeCostMs() const { return control::volumeCostMs(kCost, workCounters()); }
    control::FramePlan planFrame(double dtH) const {
        if (debugOptions().maxHydroStepsOverride > 0) return {std::min(debugOptions().maxHydroStepsOverride, 64), dtH};
        return control::planFrame(kCost, workCounters(), dtH);
    }
    int maxHydroSteps() const { return planFrame(std::max(dtHydroLimit, 1e-9)).maxSteps; }

    double escapeRadius() const { return 2.0 * nestL1 * nestRatio * nestRatio; }

    void planGeometry(double tEnd) {
        double L = nestL1;
        for (int k = 1; k < kGrids; ++k) {
            geomDep[k] = glm::dvec4(0.0, 0.0, 0.0, L);
            L *= nestRatio;
        }
        if (frozen) {
            advanceFrozen(tEnd);
            frzXDep = frzX;
        }
        if (g0Active) {
            const glm::dvec3 c = predictCore(tEnd);
            geomDep[0] = glm::dvec4(c, g0Half);
        } else {
            geomDep[0] = glm::dvec4(0.0);
        }
    }

    // Cell size of the finest nest level containing the box of grid 0 (0 if none).
    double nestCellAround(const glm::dvec4& g) const {
        for (int k = 1; k < kGrids; ++k)
            if (geomDep[k].w > 0.0 && insideBox(geomDep[k], glm::dvec3(g), g.w)) return cellOf(geomDep[k]);
        return 0.0;
    }

    void writeParams(double dt, bool last, bool monoOn) {
        ParamsUbo p{};
        for (int g = 0; g < kGrids; ++g) {
            p.gridDep[g] = glm::vec4(geomDep[g]);
            p.gridFld[g] = glm::vec4(geomFld[g]);
        }
        // Ownership ramps (tde_volume.comp): 4 own cells, and at least 1.5 cells of the grid that takes
        // over where that grid's texels carry the transition. While grid 0 feeds the nest by
        // restriction the nest renders exactly what grid 0 does not (owned words), so 4 cells suffice.
        float ramp[kGrids];
        const bool split = g0Restricts();
        for (int g = 0; g < kGrids; ++g) {
            const glm::dvec4& geo = geomDep[g];
            double coarser = 0.0;
            if (g == 0) coarser = split ? 0.0 : nestCellAround(geo);
            else if (g + 1 < kGrids) coarser = cellOf(geomDep[g + 1]);
            ramp[g] = static_cast<float>(std::max(4.0 * cellOf(geo), 1.5 * coarser));
        }
        p.ramp = glm::vec4(ramp[0], ramp[1], ramp[2], ramp[3]);
        p.event = glm::vec4(static_cast<float>(ev.q), static_cast<float>(ev.R), static_cast<float>(ev.rp), static_cast<float>(ev.rt));
        p.step = glm::vec4(static_cast<float>(dt), static_cast<float>(kEta), static_cast<float>(kMaxSubsteps), last ? 1.0f : 0.0f);
        const double cell0 = g0Active ? 2.0 * g0Half / kRes : 0.0;
        // mono.z switches the monopole statistics (histogram, core centroid, bound test); the force has its
        // own weight (extra.z).
        p.mono = glm::vec4(static_cast<float>(std::max(2.0 * g0Half, 1e-30)), static_cast<float>(0.75 * cell0),
                           (monoOn && g0Active) ? 1.0f : 0.0f, static_cast<float>(0.4 * g0Half));
        const double massUnit = std::floor(2147483648.0 / count);
        p.scale = glm::vec4(static_cast<float>(massUnit), static_cast<float>(energyScale), static_cast<float>(heatScale), static_cast<float>(count));
        p.phys = glm::vec4(noBh ? 0.0f : 1.0f, static_cast<float>(kViscosity) * debugOptions().viscosityScale * debugOptions().compressionViscosityScale,
                           static_cast<float>(kCoolingOrbits),
                           static_cast<float>(noBh ? 1e30 : escapeRadius()));
        p.limit = glm::vec4(static_cast<float>(kAccelCap), static_cast<float>(kCfl), static_cast<float>(kCAcc), static_cast<float>(kCOrb));
        p.vis = glm::vec4(static_cast<float>(rhoStar), static_cast<float>(star.teff), static_cast<float>(uStar), static_cast<float>(dissUnit));
        p.poly = glm::vec4(static_cast<float>(poly.Kp), static_cast<float>(1.0 / le.n), static_cast<float>(ev.rp), 0.0f);
        const double sg = debugOptions().selfGravity ? coreState.selfGravityWeight() : 0.0;
        p.extra = glm::vec4(debugOptions().pressureScale, static_cast<float>(kNoiseDamping) * debugOptions().viscosityScale * debugOptions().noiseDampingScale,
                            static_cast<float>(sg), 0.0f);
        p.frozen = glm::vec4(glm::vec3(frzXDep), frozen ? 1.0f : 0.0f);
        p.frozenVel = glm::vec4(glm::vec3(frzV), static_cast<float>(frzRadius));
        p.info = glm::ivec4(count, nestRatio, g0Restricts() ? 1 : 0, coreState.remnant() ? 1 : 0);
        const GLintptr off = static_cast<GLintptr>(uboSlot * uboStride);
        glNamedBufferSubData(bufUbo, off, sizeof p, &p);
        glBindBufferRange(GL_UNIFORM_BUFFER, kUboBinding, bufUbo, off, sizeof p);
        uboSlot = (uboSlot + 1) % kUboSlots;
    }

    void bindBuffers() {
        const GLuint bufs[kSsboBindings] = {bufPos, bufVel, bufAux, bufAcc, bufFA, bufFB, bufMono, bufCore, bufStats, bufBlk, bufFC};
        for (GLuint i = 0; i < kSsboBindings; ++i) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, bufs[i]);
    }
    void unbindBuffers() {
        for (GLuint i = 0; i < kSsboBindings; ++i) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, kUboBinding, 0);
        glUseProgram(0);
    }
    static void barrier() { glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT); }

    void dispatchParticles(ShaderProgram& prog) {
        prog.use();
        glDispatchCompute(static_cast<GLuint>((count + 255) / 256), 1, 1);
        barrier();
    }

    // Grid 0 feeds the nest cell by cell while it is at least twice finer than every nest level.
    bool g0Restricts() const { return geomDep[0].w > 0.0 && geomDep[1].w > 0.0 && 2.0 * cellOf(geomDep[0]) < cellOf(geomDep[1]); }

    // Zeroes the accumulators of the blocks the previous deposit used (all others are zero already).
    void clearAccumulators() {
        progBlocks.use();
        progBlocks.set("uMode", 0);
        glDispatchCompute(static_cast<GLuint>(kGrids * kBlocksPerGrid), 1, 1);
        const GLuint zero = 0;
        // Per-step parts of the Mono block: histogram and core sums (the set-up tables persist).
        glClearNamedBufferSubData(bufMono, GL_R32UI, 0, (kBins + 2) * 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glClearNamedBufferSubData(bufMono, GL_R32UI, kMonoCore * 4, 32 * 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        barrier();
    }

    // Optional per-pass timestamps (debugOptions().profilePasses).
    GLuint profQ[kTdePassCount + 2] = {};
    bool profiling = false;
    void mark(int i) {
        if (profiling) glQueryCounter(profQ[i], GL_TIMESTAMP);
    }

    // Deposit, restriction and decoding for the current deposit geometry.
    void buildGrids() {
        clearAccumulators();
        mark(3);
        dispatchParticles(progDeposit);
        mark(4);   // the restriction pass time includes grid 0's restriction into the nest
        if (g0Restricts()) {
            progG0Nest.use();
            glDispatchCompute(kRes / 8, kRes / 8, kRes / 8);
            barrier();
        }
        progRestrict.use();
        for (int k = 1; k + 1 < kGrids; ++k) {
            if (geomDep[k].w <= 0.0 || geomDep[k + 1].w <= 0.0) continue;
            progRestrict.set("uFine", k);
            progRestrict.set("uRatio", nestRatio);
            const int f3 = nestRatio * nestRatio * nestRatio;
            const int perGroup = 64 / std::min(64, f3);
            const int n = kRes / nestRatio;
            glDispatchCompute(static_cast<GLuint>((n * n * n + perGroup - 1) / perGroup), 1, 1);
            barrier();
        }
        progBlocks.use();
        progBlocks.set("uMode", 1);
        glDispatchCompute(static_cast<GLuint>((kGrids * kBlocksPerGrid + 255) / 256), 1, 1);
        barrier();
        mark(5);
        progCells.use();
        for (int g = 0; g < kGrids; ++g) {
            if (geomDep[g].w <= 0.0) continue;
            progCells.set("uGrid", g);
            glDispatchCompute(kRes / 8, kRes / 8, kRes / 8);
        }
        barrier();
        mark(6);
    }

    void initPass(int mode, bool single = false, glm::vec3 a = glm::vec3(0.0f), glm::vec3 b = glm::vec3(0.0f)) {
        progInit.use();
        progInit.set("uMode", mode);
        progInit.set("uRelPos", mode == 0 ? glm::vec3(comX) : a);
        progInit.set("uRelVel", mode == 0 ? glm::vec3(comV) : b);
        glDispatchCompute(single ? 1u : static_cast<GLuint>((count + 255) / 256), 1, 1);
        barrier();
    }

    void corePass(int mode) {
        progCore.use();
        progCore.set("uMode", mode);
        glDispatchCompute(1, 1, 1);
        barrier();
    }

    // ============================================================ rigid phase
    // Advances the intact star; returns the time left over after a release (0 if none).
    double advanceRigid(double dt) {
        double t = 0.0;
        if (noBh) {
            release();
            return dt;
        }
        for (int it = 0; it < 1000000 && t < dt; ++it) {
            const double r = glm::length(comX);
            if (!ev.swallowed && r <= ev.rRelease) break;
            const double ts = phys::bhTimescale(r, phys::angularMomentum2(comX, comV), glm::length(comV));
            const double chunk = std::min(dt - t, 0.02 * ts / lapse(comX, comV));
            bool accreted = false;
            advanceBallistic(comX, comV, chunk, &accreted);
            t += chunk;
            time += chunk;
            if (accreted) {
                finished = true;
                stats.accretedFraction = 1.0;
                volCount = 0;
                logInfo("star disruption: the intact star crossed the horizon at t = {:.4g} M", time);
                return 0.0;
            }
        }
        if (!ev.swallowed && glm::length(comX) <= ev.rRelease) {
            if (initialized) release();
            else finished = true;
            return dt - t;
        }
        return 0.0;
    }

    void release() {
        released = true;
        stats.released = true;
        g0Active = true;
        g0Half = kG0InitialHalf * ev.R;
        g0Starved = 0;
        coreX = comX;
        coreV = comV;
        coreT = time;
        coreValid = true;
        // The whole star is bound to itself.
        coreState.reset();
        boundMass = 1.0;
        for (int g = 0; g < kGrids; ++g) sinceActivation[g] = 0;
        planGeometry(time);
        for (auto& g : geomFld) g = glm::dvec4(0.0);

        // Core buffer: state and a static trajectory at the centre of mass.
        std::vector<glm::vec4> core(5 + kTraj + 1, glm::vec4(glm::vec3(comX), 0.0f));
        core[0] = glm::vec4(glm::vec3(comX), 1.0f);
        core[1] = glm::vec4(glm::vec3(comV), static_cast<float>(count));
        core[2] = glm::vec4(0.0f);
        core[3] = glm::vec4(glm::vec3(comV), 0.0f);
        core[4] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);   // round
        glNamedBufferSubData(bufCore, 0, static_cast<GLsizeiptr>(core.size() * sizeof(glm::vec4)), core.data());
        tRelease = time;

        bindBuffers();
        writeParams(0.0, false, false);
        initPass(0);
        // Densities, the monopole table and the core centroid (energies are still zero).
        writeParams(0.0, false, true);
        buildGrids();
        corePass(0);
        for (int g = 0; g < kGrids; ++g) geomFld[g] = geomDep[g];
        writeParams(0.0, false, true);
        // Hydrostatic internal energies (tde_init.comp), then the grids with them; three corrections
        // of the pressure profile against the net force that remains.
        const GLuint zero = 0;
        glClearNamedBufferSubData(bufMono, GL_R32UI, kMonoRho * 4, (kMonoWords - kMonoRho) * 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        initPass(1);
        initPass(2);
        initPass(3, true);
        initPass(4);
        buildGrids();
        corePass(0);
        for (int g = 0; g < kGrids; ++g) geomFld[g] = geomDep[g];
        for (int it = 0; it < 3; ++it) {
            writeParams(0.0, false, true);
            glClearNamedBufferSubData(bufMono, GL_R32UI, kMonoAr * 4, 2 * kBins * 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            initPass(5);
            initPass(6, true);
            initPass(4);
            buildGrids();
            corePass(0);
        }

        // First hydro step from the star's structure: CFL at the central sound speed, a fraction of the
        // dynamical time and of the orbital time.
        const double cs = std::sqrt(phys::kGamma * poly.pOverRhoC);
        const double cell0 = 2.0 * g0Half / kRes;
        const double r = std::max(glm::length(comX), 1.0);
        dtHydroLimit = std::min({kCfl * cell0 / cs, 0.05 * tStar, noBh ? 1e30 : kCOrb * 2.0 * phys::kPi * std::pow(r, 1.5)});
        logInfo("star disruption: released at r = {:.4g} M, t = {:.4g} M (first hydro step {:.3g} M)", glm::length(comX), time, dtHydroLimit);
    }

    // ============================================================ readback (two frames behind)
    // Remnant classification of the bound fraction and the self-gravity weight from the core state of the
    // frame in `slot` (control::CoreState, TdeControl_internal.h).
    void updateRemnantState(const ReadbackData& rb, int slot) {
        boundMass = std::max(static_cast<double>(rb.core[11]), 0.0);
        g_debugInfo.boundNearCore = rb.core[9] / static_cast<double>(count);
        g_debugInfo.elongation = rb.core[10];
        g_debugInfo.peakDensity = rb.core[8] / std::max(rhoStar, 1e-300);
        control::CoreObservation o;
        o.t = ringTime[slot];
        o.frameDt = ringDt[slot];
        o.g0Active = g0Active;
        o.coreMode = coreMode;
        o.frozen = frozen;
        o.coreValid = coreValid;
        o.outbound = glm::dot(coreX, coreV) > 0.0;
        o.boundMass = boundMass;
        control::CoreParams p;
        p.tStar = tStar;
        p.remnantDelay = kRemnantDelay;
        p.selfGravityBound = kSelfGravityBound;
        p.selfGravityOffDelay = kSelfGravityOffDelay;
        p.selfGravityFade = kSelfGravityFade;
        p.fadeEnabled = debugOptions().selfGravityFade;
        p.blackHole = !noBh;
        coreState.observe(o, p);
    }

    void readback() {
        if (gpuFrame < 2) return;
        const int slot = static_cast<int>((gpuFrame - 2) % kRing);
        if (!ringValid[slot]) return;
        ringValid[slot] = false;
        if (fence[slot]) {
            glClientWaitSync(fence[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
            glDeleteSync(fence[slot]);
            fence[slot] = nullptr;
        }
        ReadbackData rb{};
        glGetNamedBufferSubData(bufReadback[slot], 0, sizeof rb, &rb);
        if (ringTimed[slot]) {
            GLuint64 t0 = 0, t1 = 0;
            glGetQueryObjectui64v(query[2 * slot], GL_QUERY_RESULT, &t0);
            glGetQueryObjectui64v(query[2 * slot + 1], GL_QUERY_RESULT, &t1);
            g_debugInfo.gpuMs = t1 > t0 ? static_cast<double>(t1 - t0) * 1e-6 : 0.0;
        }
        const double n = static_cast<double>(count);
        // Cumulative counters: valid whatever happened since the frame was recorded.
        stats.accretedFraction = rb.words[S_ACCRETED] / n;
        stats.escapedFraction = rb.words[S_ESCAPED] / n;
        g_debugInfo.nonFiniteParticles = static_cast<int>(rb.words[S_NAN]);
        if (!ringHadSteps[slot]) return;
        // A freeze, thaw or removal of the remnant after this frame was recorded changed which particles
        // are active and which belong to the remnant (the frozen ones are added here, the active ones were
        // counted on the GPU): the frame's particle counts, its "no gas left" test and its time-step
        // histogram (which does not know the thawed gas) are not used; the HUD keeps the previous counts
        // for those (at most two) frames.
        const bool sameEpoch = ringEpoch[slot] == remnantEpoch;
        if (sameEpoch) {
            if (frozen) frozenCount += static_cast<int>(rb.words[S_FROZEN]);   // gas that fell into the remnant
            const bool frozenBound = frozen && !noBh && phys::specificEnergy(frzX, frzV) < 1.0;
            stats.activeParticles = static_cast<int>(rb.words[S_ACTIVE]) + (frozen ? frozenCount : 0);
            stats.boundFraction = (rb.words[S_BOUND] + (frozenBound ? frozenCount : 0)) / n;
        }
        const double diss = (static_cast<double>(rb.words[S_DISS + 1]) * 4294967296.0 + rb.words[S_DISS]) * dissUnit / n;
        const double rate = ringDt[slot] > 0.0 ? diss / ringDt[slot] : 0.0;
        stats.dissipationRel = noBh ? 0.0 : rate * tMin * 4.0 * ev.rp;
        g_debugInfo.cappedParticles = static_cast<int>(rb.words[S_CAPPED]);
        for (int g = 0; g < kGrids; ++g) {
            occ[g] = static_cast<int>(rb.words[S_OCC + g]);
            g_debugInfo.occupiedCells[g] = occ[g];
        }
        liveBlocks = static_cast<int>(rb.words[S_LIVE]);
        g_debugInfo.liveBlocks = liveBlocks;
        // Work of the frame's last hydro step: the cost model's inputs.
        substepsPerStep = rb.words[S_SUBSTEPS];
        writesPerStep = rb.words[S_GWRITES];
        hMeasured = ringSteps[slot] > 0 ? ringDt[slot] / ringSteps[slot] : 0.0;
        haveCounters = true;
        g_debugInfo.readbackSteps = ringSteps[slot];
        g_debugInfo.readbackActive = static_cast<double>(rb.words[S_ACTIVE]);
        g_debugInfo.readbackBlocks = static_cast<double>(liveBlocks);
        if (sameEpoch && stats.activeParticles == 0 && released) {
            finished = true;
            volCount = 0;
            logInfo("star disruption: no gas left at t = {:.4g} M (accreted {:.3f}, escaped {:.3f})", time,
                    stats.accretedFraction, stats.escapedFraction);
            return;
        }

        // Hydro step limit: all but kDtTailFraction of the particles are satisfied.
        uint64_t total = 0;
        for (int b = 0; sameEpoch && b < kDtBins; ++b) total += rb.words[S_DTHIST + b];
        if (total > 0) {
            const double target = kDtTailFraction * static_cast<double>(total);
            uint64_t cum = 0;
            double allowed = 0.0;
            for (int b = 0; b < kDtBins; ++b) {
                cum += rb.words[S_DTHIST + b];
                if (static_cast<double>(cum) > target) {
                    allowed = std::exp2((b - 64) * 0.5);
                    break;
                }
            }
            if (allowed > 0.0) dtHydroLimit = std::min(allowed, dtHydroLimit > 0.0 ? 1.5 * dtHydroLimit : allowed);
        }

        // Core tracker and the size of the debris grid.
        coreValid = rb.core[3] > 0.5f;
        coreParticles = rb.core[7];
        if (coreValid) {
            coreX = glm::dvec3(rb.core[0], rb.core[1], rb.core[2]);
            coreV = glm::dvec3(rb.core[4], rb.core[5], rb.core[6]);
            coreT = ringTime[slot];
        }
        // Debris grid policy. While a core survives (see below) the grid keeps 1.4 stellar radii, enough
        // to hold the star with its force margin and to resolve a remnant. Without one it follows the
        // densest part of the stream and grows while the outer eighth of the box holds more than 2% of
        // its gas, but never faster than the debris can spread (half the velocity spread
        // sqrt(2 R) / r_t); it relaxes back otherwise.
        if (g0Active) {
            const double mass = rb.words[S_G0_MASS] / 256.0;
            const double outer = rb.words[S_G0_OUTER] / 256.0;
            const double frac = mass > 0.0 ? outer / mass : 1.0;
            // A core survives while a fraction kCoreBound of the star near the density centre is
            // gravitationally bound to it, that gas is round, and its amount has not dropped by more
            // than a quarter over the last dynamical time of the star (the middle of a stretching
            // stream drains away, a remnant does not).
            const double boundToCore = rb.core[9];
            const double elongation = rb.core[10];
            coreHistory[historyHead] = {ringTime[slot], coreParticles};
            historyHead = (historyHead + 1) % kHistory;
            double before = coreParticles;
            for (const auto& h : coreHistory)
                if (h.first > 0.0 && h.first <= ringTime[slot] - tStar && h.first > ringTime[slot] - 3.0 * tStar) before = std::max(before, h.second);
            const bool stable = coreParticles >= 0.75 * before;
            coreMode = frozen || (boundToCore >= kCoreBound * n && elongation < 0.5 && stable);
            g_debugInfo.g0OuterFraction = frac;
            const double base = kG0InitialHalf * ev.R;
            const double cap = base + 0.5 * std::sqrt(2.0 * ev.R) / ev.rt * (time - tRelease);
            if (coreMode) g0Half = std::max(base, g0Half * 0.97);
            else if (frac > 0.02) g0Half = std::min(g0Half * 1.15, std::max(cap, g0Half));
            else if (frac < 0.002) g0Half = std::max(base, g0Half * 0.97);
            g0Starved = (mass < 0.01 * n || !coreValid) ? g0Starved + 1 : 0;
            const glm::dvec4 g0(predictCore(time), g0Half);
            const double nestCell = nestCellAround(g0);
            const bool coarse = nestCell > 0.0 && 2.0 * g0Half / kRes > 0.7 * nestCell;
            const bool huge = !noBh && g0Half > nestL1 * nestRatio * nestRatio;
            if (!frozen && (g0Starved >= 3 || coarse || huge)) {
                g0Active = false;
                logInfo("star disruption: debris grid retired at t = {:.4g} M ({})", time,
                        g0Starved >= 3 ? "no dense core left" : (coarse ? "no finer than the black hole grids" : "too large"));
            }
        }
        updateRemnantState(rb, slot);
        // Freeze a surviving core once it has left the tidal zone; thaw it if it comes back; drop it
        // when it is unbound and far away.
        if (!noBh) {
            if (!frozen && g0Active && coreMode && coreValid && glm::dot(coreX, coreV) > 0.0 && glm::length(coreX) > quietRadius() &&
                time >= freezeRetryTime)
                pendingFrozenOp = 7;
            if (frozen) {
                advanceFrozen(time);
                if (glm::length(frzX) < 0.8 * quietRadius() && glm::dot(frzX, frzV) < 0.0) pendingFrozenOp = 8;
                else if (glm::length(frzX) > escapeRadius() && phys::specificEnergy(frzX, frzV) >= 1.0) pendingFrozenOp = 9;
            }
        }
    }

    // Applies a pending freeze / thaw / removal of the remnant (start of a GPU frame, buffers bound).
    void applyFrozenOp() {
        if (frozen && debugOptions().forceRemnantOp != 0) {
            pendingFrozenOp = debugOptions().forceRemnantOp;
            debugOptions().forceRemnantOp = 0;
        }
        const int op = pendingFrozenOp;
        pendingFrozenOp = 0;
        if (op == 0) return;
        planGeometry(time);
        writeParams(0.0, false, true);
        uint32_t changed = 0;
        if (op == 7) {
            // Particles near the core centre and slower than 3 times the core's escape speed.
            const double dc = 0.4 * g0Half;
            const double vesc = std::sqrt(2.0 * ev.q * std::max(coreParticles, 1.0) / count / dc);
            initPass(7, false, glm::vec3(0.0f), glm::vec3(static_cast<float>(0.75 * g0Half), static_cast<float>(3.0 * vesc), 0.0f));
            float core[8];
            glGetNamedBufferSubData(bufCore, 0, sizeof core, core);
            glGetNamedBufferSubData(bufStats, S_FROZEN * 4, 4, &changed);
            if (changed == 0) freezeRetryTime = time + tStar;   // try again a dynamical time later
            if (changed > 0) {
                frozen = true;
                ++remnantEpoch;
                dtAtFreeze = dtHydroLimit;
                frzX = glm::dvec3(core[0], core[1], core[2]);
                frzV = glm::dvec3(core[4], core[5], core[6]);
                frzT = time;
                frzXDep = frzX;
                frzRadius = 0.75 * g0Half;
                frozenCount = static_cast<int>(changed);
                logInfo("star disruption: remnant of {} particles frozen at r = {:.4g} M, t = {:.4g} M", changed, glm::length(frzX), time);
            }
        } else if (op == 8 || op == 9) {
            advanceFrozen(time);
            frzXDep = frzX;
            writeParams(0.0, false, true);
            initPass(op);
            glGetNamedBufferSubData(bufStats, S_FROZEN * 4, 4, &changed);
            logInfo("star disruption: remnant of {} particles {} at r = {:.4g} M, t = {:.4g} M", changed,
                    op == 8 ? "thawed" : "removed (unbound, far away)", glm::length(frzX), time);
            coreX = frzX;
            coreV = frzV;
            coreT = time;
            frozen = false;
            frozenCount = 0;
            ++remnantEpoch;
            // The thawed gas needs the step it had when it froze; the histograms of the frames in flight
            // do not know it yet.
            if (op == 8 && dtAtFreeze > 0.0) dtHydroLimit = std::min(dtHydroLimit, dtAtFreeze);
            if (op == 9) g0Active = false;
        }
        const GLuint zero = 0;
        glClearNamedBufferSubData(bufStats, GL_R32UI, S_FROZEN * 4, 4, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    }

    // ============================================================ GPU frame
    void runGpuFrame(double dt) {
        readback();
        if (finished) return;
        const control::FrameSteps fs = control::frameSteps(planFrame(std::max(dtHydroLimit, 1e-9)), dt);
        if (fs.limited) stats.limited = true;
        const int steps = fs.steps;
        const double h = fs.h;
        const double dtFrame = fs.advance;
        lastSteps = steps;
        lastDt = h;

        const int slot = static_cast<int>(gpuFrame % kRing);
        if (fence[slot]) {
            glDeleteSync(fence[slot]);
            fence[slot] = nullptr;
        }
        const bool timed = debugOptions().measureGpuTime;
        if (timed) glQueryCounter(query[2 * slot], GL_TIMESTAMP);
        bindBuffers();
        const GLuint zero = 0;
        glClearNamedBufferSubData(bufStats, GL_R32UI, kStatCumulative * 4, (kStatWords - kStatCumulative) * 4, GL_RED_INTEGER,
                                  GL_UNSIGNED_INT, &zero);
        applyFrozenOp();
        const bool profile = debugOptions().profilePasses;
        if (profile && !profQ[0]) glGenQueries(kTdePassCount + 2, profQ);
        for (int k = 0; k < steps; ++k) {
            const bool last = k == steps - 1;
            profiling = profile && k == 0;
            planGeometry(time + h);
            writeParams(h, last, true);
            mark(0);
            corePass(1);
            mark(1);
            dispatchParticles(progUpdate);
            mark(2);
            buildGrids();
            corePass(0);
            mark(7);
            profiling = false;
            for (int g = 0; g < kGrids; ++g) {
                if (geomDep[g].w > 0.0 && geomFld[g].w <= 0.0) sinceActivation[g] = 0;
                geomFld[g] = geomDep[g];
            }
            time += h;
        }
        // Rendering volumes from the last deposit.
        writeParams(h, false, true);
        if (profile && profQ[0]) glQueryCounter(profQ[8], GL_TIMESTAMP);
        // The coarsest nest level goes first: it also writes the particle counts that every other grid's
        // noise fade reads (tde_volume.comp).
        progVolume.use();
        const int fadeLevel = geomFld[kGrids - 1].w > 0.0 ? kGrids - 1 : -1;
        progVolume.set("uFadeLevel", fadeLevel);
        progVolume.set("uUnweighted", debugOptions().unweightedVolumes ? 1 : 0);
        volCount = 0;
        for (int k = 0; k < kGrids; ++k) {
            const int g = fadeLevel >= 0 ? (fadeLevel + k) % kGrids : k;   // reference level first
            if (geomFld[g].w <= 0.0) continue;
            const bool reference = g == fadeLevel;
            glBindImageTexture(0, textures[g], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            if (reference) {
                glBindImageTexture(1, texFadeRef, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R16F);
            } else {
                glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
                glBindTextureUnit(15, texFadeRef);
            }
            progVolume.set("uGrid", g);
            glDispatchCompute(kRes / 8, kRes / 8, kRes / 8);
            if (reference) glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            ++sinceActivation[g];
        }
        glBindTextureUnit(15, 0);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        for (int g = 0; g < kGrids; ++g) {
            if (geomFld[g].w <= 0.0 || !(occ[g] > 0 || sinceActivation[g] <= 3)) continue;
            GasVolume& v = vols[static_cast<size_t>(volCount++)];
            v.texture = textures[g];
            v.center = glm::vec3(geomFld[g]);
            v.halfSize = static_cast<float>(geomFld[g].w);
            v.resolution = kRes;
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        if (profile && profQ[0]) {
            glQueryCounter(profQ[9], GL_TIMESTAMP);
            glFinish();
            GLuint64 ts[kTdePassCount + 2] = {};
            for (int i = 0; i < kTdePassCount + 2; ++i) glGetQueryObjectui64v(profQ[i], GL_QUERY_RESULT, &ts[i]);
            for (int i = 0; i < kTdePassCount - 1; ++i)
                g_debugInfo.passMs[i] = (steps > 0 && ts[i + 1] > ts[i]) ? static_cast<double>(ts[i + 1] - ts[i]) * 1e-6 : 0.0;
            g_debugInfo.passMs[kTdePassCount - 1] = ts[9] > ts[8] ? static_cast<double>(ts[9] - ts[8]) * 1e-6 : 0.0;
            // The work counters of this frame's last step (the profiled one when there is a single step).
            uint32_t w[kStatWords];
            glGetNamedBufferSubData(bufStats, 0, sizeof w, w);
            g_debugInfo.profSubsteps = w[S_SUBSTEPS];
            g_debugInfo.profWrites = w[S_GWRITES];
            g_debugInfo.profActive = w[S_ACTIVE] + (frozen ? frozenCount : 0);
            g_debugInfo.profBlocks = w[S_LIVE];
        }
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glCopyNamedBufferSubData(bufStats, bufReadback[slot], 0, 0, kStatWords * 4);
        glCopyNamedBufferSubData(bufCore, bufReadback[slot], 0, kStatWords * 4, 48);
        if (timed) glQueryCounter(query[2 * slot + 1], GL_TIMESTAMP);
        fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        ringValid[slot] = true;
        ringHadSteps[slot] = steps > 0;
        ringTimed[slot] = timed;
        ringTime[slot] = time;
        ringDt[slot] = dtFrame;
        ringEpoch[slot] = remnantEpoch;   // after this frame's remnant operation (applyFrozenOp above)
        ringSteps[slot] = steps;
        g_debugInfo.predictedFrameMs = steps * stepCostMs(h) + volumeCostMs();
        ++gpuFrame;
        unbindBuffers();
    }

    void step(double dtM) {
        if (!spawned || finished) return;
        if (!(dtM > 0.0) || !std::isfinite(dtM)) return;
        dtM = std::min(dtM, 1e13);
        stats.limited = false;
        double remaining = dtM;
        if (!released) {
            remaining = advanceRigid(dtM);
            if (finished || !released) {
                stats.timeM = time;
                updateDebug();
                return;
            }
        }
        if (!initialized) return;
        runGpuFrame(remaining);
        stats.timeM = time;
        updateDebug();
    }

    void updateDebug() {
        g_debugInfo.hydroSteps = lastSteps;
        g_debugInfo.hydroDt = lastDt;
        g_debugInfo.hydroDtLimit = dtHydroLimit;
        g_debugInfo.maxHydroSteps = maxHydroSteps();
        for (int g = 0; g < kGrids; ++g) g_debugInfo.grid[g] = glm::vec4(geomFld[g]);
        g_debugInfo.corePos = coreX;
        g_debugInfo.coreValid = coreValid;
        g_debugInfo.coreParticles = coreParticles;
        g_debugInfo.particleBuffers[0] = bufPos;
        g_debugInfo.particleBuffers[1] = bufVel;
        g_debugInfo.particleBuffers[2] = bufAux;
        g_debugInfo.particleCount = count;
        g_debugInfo.coreMode = coreMode;
        g_debugInfo.remnant = coreState.remnant();
        g_debugInfo.frozen = frozen;
        g_debugInfo.boundMass = boundMass;
        g_debugInfo.selfGravityWeight = debugOptions().selfGravity ? coreState.selfGravityWeight() : 0.0;
        g_debugInfo.predictedStepMs = stepCostMs(std::max(dtHydroLimit, 1e-9));
        g_debugInfo.substepsPerStep = substepsPerStep;
        g_debugInfo.writesPerStep = writesPerStep;
    }

    glm::dvec3 position() {
        if (!released) return comX;
        return predictCore(time);
    }
    glm::dvec3 velocity() {
        if (!released) return comV * lapse(comX, comV);
        predictCore(time);
        return coreV * lapse(coreX, coreV);
    }
};

// ================================================================================================
TdeSim::TdeSim() : impl_(std::make_unique<Impl>()) {}
TdeSim::~TdeSim() = default;

bool TdeSim::init(int maxParticles) { return impl_->init(maxParticles); }
void TdeSim::shutdown() { impl_->shutdown(); }
void TdeSim::spawn(const StarParams& star, const OrbitParams& orbit, double bhMassSolar, int particles) {
    impl_->spawn(star, orbit, bhMassSolar, particles);
}
void TdeSim::clear() { impl_->clear(); }
bool TdeSim::active() const { return impl_->spawned && !impl_->finished; }
void TdeSim::step(double dtM) { impl_->step(dtM); }
const TdeStats& TdeSim::stats() const { return impl_->stats; }
int TdeSim::volumeCount() const { return (impl_->spawned && !impl_->finished) ? impl_->volCount : 0; }
const GasVolume& TdeSim::volume(int i) const {
    static const GasVolume empty{};
    if (i < 0 || i >= volumeCount()) return empty;
    return impl_->vols[static_cast<size_t>(i)];
}
bool TdeSim::starIntact() const { return impl_->spawned && !impl_->released && !impl_->finished; }
glm::dvec3 TdeSim::starPosition() const { return impl_->position(); }
glm::dvec3 TdeSim::starVelocity() const { return impl_->velocity(); }
double TdeSim::starRadiusM() const { return impl_->ev.R; }
double TdeSim::starTeff() const { return impl_->star.teff; }

}  // namespace bhs::tde
