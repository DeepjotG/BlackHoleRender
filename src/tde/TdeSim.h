#pragma once
// Tidal disruption of a star by the black hole, simulated on the GPU with OpenGL 4.6 compute
// shaders (shaders/tde_*.comp).
//
// Everything is in the renderer frame and geometric units (Units.h): black hole of mass 1 at the
// origin, +Y = spin axis, lengths and times in M. The star's mass in these units is
// q = M_star / M_bh (tiny), its radius R = R_star / metersPerM.
//
// Life cycle: spawn() places an intact star on a parabolic orbit with pericentre r_p = r_t / beta,
// where r_t = R * q^(-1/3) is the tidal radius. The star moves as a rigid body until it gets close
// (about 2 r_t, or pericentre if that is farther), then its particles are released and evolve
// individually under the black hole's gravity (with the relativistic correction that reproduces
// Schwarzschild orbit shapes, precession, the ISCO and plunges), the star's self-gravity, gas
// pressure and dissipation. Debris stretches into a stream; bound debris returns, collides with
// itself, heats up and settles into a disk; particles that cross the horizon are accreted.
// If r_p is inside the capture radius (4 M for a parabolic orbit) the star is swallowed whole.
//
// Rendering contract: up to kMaxVolumes cube volumes, each a GL_TEXTURE_3D (GL_RGBA16F, linear
// filtering, clamp to edge) with
//   r = absorption coefficient (optical depth per M of path),
//   g = gas temperature / 1e4 K,
//   b = emission strength multiplier (1 = blackbody at that temperature, more where shocks heat),
//   a = reserved (0).
// The ray tracer integrates color += T * (1 - exp(-r ds)) * B(g * 1e4 K) * b and T *= exp(-r ds).
// Volumes may overlap (e.g. one around the black hole, one following the debris); the tracer adds
// them. Texel (i, j, k) covers center + halfSize * (2 * ((i, j, k) + 0.5) / resolution - 1).
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <memory>

namespace bhs::tde {

struct StarParams {
    double massSolar = 1.0;
    double radiusSolar = 1.0;
    double teff = 5772.0;           // K, colour of the intact star
    double polytropeN = 3.0;        // 1.5 for convective stars (giants), 3 for Sun-like stars
};

struct OrbitParams {
    double beta = 1.0;              // penetration factor r_t / r_p
    double inclinationDeg = 20.0;   // tilt of the orbit plane relative to the accretion disk (XZ)
    double longitudeDeg = 0.0;      // direction of the pericentre around +Y
    bool retrograde = false;        // orbit sense relative to the disk's rotation (+Y angular momentum)
    double startDistanceRt = 3.0;   // initial distance in tidal radii
};

struct TdeStats {
    double timeM = 0.0;             // simulated time since spawn, M units
    double tidalRadiusM = 0.0;
    double pericenterM = 0.0;
    double starRadiusM = 0.0;
    bool swallowedWhole = false;    // pericentre inside the capture radius: no disruption
    bool released = false;          // particles evolving individually
    double boundFraction = 0.0;     // of the star's mass: bound to the black hole
    double accretedFraction = 0.0;  // crossed the horizon
    double escapedFraction = 0.0;   // unbound and far away
    int activeParticles = 0;
    double dissipationRel = 0.0;    // current heating rate, relative units (HUD "flare" meter)
    bool limited = false;           // the requested dt was reduced to keep the cost bounded
};

struct GasVolume {
    GLuint texture = 0;
    glm::vec3 center{0.0f};         // M units, renderer frame
    float halfSize = 0.0f;          // M units
    int resolution = 0;
};

class TdeSim {
public:
    static constexpr int kMaxVolumes = 4;

    TdeSim();
    ~TdeSim();
    TdeSim(const TdeSim&) = delete;
    TdeSim& operator=(const TdeSim&) = delete;

    // Compiles the compute shaders (through bhs::ShaderProgram, shader dir already set) and
    // allocates buffers for up to `maxParticles`. Requires a current OpenGL 4.6 context.
    bool init(int maxParticles = 262144);
    void shutdown();

    void spawn(const StarParams& star, const OrbitParams& orbit, double bhMassSolar, int particles = 131072);
    void clear();
    bool active() const;

    // Advances the simulation by dtM (M units). Substeps internally; if the requested dt would need
    // more work than the budget (about 3 ms of GPU time at 262k particles on an RTX 3060), it
    // advances less and sets stats().limited.
    void step(double dtM);

    const TdeStats& stats() const;
    int volumeCount() const;
    const GasVolume& volume(int i) const;

    // The intact star (before release) is drawn by the ray tracer as a glowing sphere.
    bool starIntact() const;
    glm::dvec3 starPosition() const;   // M units
    glm::dvec3 starVelocity() const;   // M per M
    double starRadiusM() const;
    double starTeff() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bhs::tde
