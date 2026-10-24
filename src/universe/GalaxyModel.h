#pragma once
// Parametric model of a galaxy's stars and dust, shared by the real Milky Way, invented host
// galaxies and every other galaxy in the 3D universe. Implemented in GalaxyModel.cpp (CPU, used for
// procedural star generation) and shaders/galaxy_model.glsl (GPU, used for the diffuse sky bake).
// The two implementations must agree (see tests/galaxy_model_tests.cpp).
//
// Disk-frame coordinates of a point p (meters, galactic frame of the universe):
//   d = p - center;  y = dot(d, normal);  x = dot(d, major);  z = dot(d, cross(normal, major))
//   cylindrical R = sqrt(x^2 + z^2), azimuth theta = atan2(z, x), measured counter-clockwise around
//   +normal starting at `major`.
// Logarithmic spiral arms: arm phase psi = armCount * (theta - ln(R / armRefRadiusPc) / tan(pitch)
//   - armPhaseRad); density is enhanced where cos(psi) ~ 1. With pitch > 0 the arms wind
//   counter-clockwise outward (seen from +normal), i.e. they trail a clockwise rotation, as in the
//   Milky Way seen from the north galactic pole.
#include <glm/glm.hpp>

#include <cstdint>

namespace bhs::universe {

enum class GalaxyType : int32_t { Spiral = 0, BarredSpiral = 1, Lenticular = 2, Elliptical = 3, Irregular = 4 };

struct GalaxyModel {
    GalaxyType type = GalaxyType::Spiral;
    uint32_t seed = 1;                   // per-galaxy detail noise
    glm::dvec3 center{0.0};              // meters, galactic frame
    glm::dvec3 normal{0.0, 1.0, 0.0};    // unit disk axis
    glm::dvec3 major{1.0, 0.0, 0.0};     // unit in-plane reference axis, perpendicular to normal

    double stellarMassSolar = 6e10;      // total stellar mass (sets star counts)
    double luminositySolar = 2.5e10;     // total V-band luminosity (sets diffuse brightness)

    // Thin exponential disk (parsecs).
    double diskScaleLengthPc = 2600.0;
    double diskScaleHeightPc = 300.0;
    double diskCutoffPc = 15000.0;       // smooth truncation radius

    // Bulge (flattened, roughly de Vaucouleurs; parsecs).
    double bulgeMassFraction = 0.15;
    double bulgeRadiusPc = 700.0;        // half-mass radius
    double bulgeFlattening = 0.6;        // polar / equatorial axis ratio

    // Bar (half-length; 0 disables). Angle measured counter-clockwise around +normal from `major`.
    double barLengthPc = 0.0;
    double barAxisRatio = 0.4;
    double barAngleDeg = 0.0;

    // Logarithmic spiral arms (see header comment for the phase convention).
    int armCount = 2;
    double armPitchDeg = 12.0;
    double armRefRadiusPc = 4000.0;
    double armPhaseDeg = 0.0;
    double armWidth = 0.4;               // 0..1, fraction of the inter-arm spacing
    double armContrast = 3.0;            // on-arm / inter-arm density ratio of young stars and dust

    // Dust layer (follows the arms).
    double dustScaleHeightPc = 100.0;
    double dustExtinctionPerKpc = 1.0;   // A_V per kpc in the midplane at R = diskScaleLength

    // Colours (linear RGB, relative weights).
    glm::vec3 bulgeColor{1.0f, 0.80f, 0.58f};
    glm::vec3 diskColor{0.92f, 0.90f, 1.0f};
    glm::vec3 armColor{0.62f, 0.74f, 1.0f};
    glm::vec3 hiiColor{1.0f, 0.40f, 0.52f};

    // Ellipticals and irregulars (parsecs).
    double ellipticalRadiusPc = 4000.0;  // effective radius
    double ellipticalAxisRatio = 0.75;

    // Number density of stars (all types, stars per pc^3) at `posMeters` (galactic frame).
    double stellarDensity(const glm::dvec3& posMeters) const;
    // Fraction of the local stars that are young/blue (arms, star-forming regions), 0..1.
    double youngFraction(const glm::dvec3& posMeters) const;
    // V-band visual extinction per parsec (A_V / pc) at `posMeters`.
    double dustExtinction(const glm::dvec3& posMeters) const;
    // Unresolved starlight: luminosity density in L_sun / pc^3 as linear RGB weights.
    glm::vec3 emissivity(const glm::dvec3& posMeters) const;
    // Radius (parsecs) outside which density, light and dust are negligible.
    double boundingRadiusPc() const;
};

// GPU twin consumed by shaders/galaxy_model.glsl. Positions are relative to a viewpoint and in kpc so
// that single precision is enough; orientation vectors are in the galactic frame.
struct GalaxyGpu {
    glm::vec4 data[16];
};
static_assert(sizeof(GalaxyGpu) == 256, "GalaxyGpu must match the std430 layout in galaxy_model.glsl");

GalaxyGpu packGalaxyGpu(const GalaxyModel& g, const glm::dvec3& viewpointMeters);

}  // namespace bhs::universe
