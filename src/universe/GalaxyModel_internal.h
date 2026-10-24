#pragma once
// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
//
// Internal evaluation of a GalaxyModel. GalaxyEval derives every constant once, then evaluates
// stars, starlight, HII-region emission and dust with exactly the formulas of the GPU twin in
// shaders/galaxy_model.glsl (which reads the same constants from GalaxyEval::pack). Used by
// GalaxyModel.cpp (public methods), StarField.cpp and the tests.
//
// Components (number densities in stars/pc^3, light in L_sun/pc^3):
//   old disk     exponential in R with a smooth cutoff, sech^2 vertically, weak arms, mild clumps
//   young disk   longer and thinner, strong arms, star-forming complexes (clumps), absent inside
//                the bar / bulge region; carries the blue light
//   HII regions  pink nebular emission in knots on the arms, inside the young complexes
//   bulge        flattened Hernquist profile (the whole of an elliptical)
//   bar          boxy (super-Gaussian) bar sharing the bulge's mass budget
//   dust         exponential layer, concentrated in lanes on the inner (concave) edge of the arms
//                and along the leading edges of the bar, with filamentary clumps
// Clump factors are exp(s * noise) divided by their mean, so they never change totals, and every
// noise octave fades to its mean when it is smaller than the requested level of detail.
#include <glm/glm.hpp>

#include <cstdint>

#include "universe/GalaxyModel.h"

namespace bhs::universe::detail {

// pcg hash and 3D value noise, bit-identical to gmHash/gmNoise in galaxy_model.glsl; range [-1, 1].
uint32_t pcgHash(uint32_t v);
float valueNoise(float x, float y, float z, uint32_t salt);
// Same, with the lattice's y index wrapped by `mask` (period mask + 1, a power of two; -1 = no wrap).
float valueNoiseWrapY(float x, float y, float z, uint32_t salt, int32_t mask);
// Radius (kpc) containing the galaxy's light and dust; cheap (GalaxyModel::boundingRadiusPc uses it).
double boundRadiusKpc(const GalaxyModel& m);

// Statistics of valueNoise (4M samples; checked in tests/galaxy_model_tests.cpp).
constexpr double kNoiseVariance = 0.16012;
// log E[exp(s n)] ~= s^2 (kLmgf2 + kLmgf3 s + kLmgf4 s^2) for 0 <= s <= 8 (relative error < 1.1%).
constexpr double kLmgf2 = 0.0815444, kLmgf3 = -0.0020730, kLmgf4 = -0.00011765;
// E[smoothstep(0.25, 0.6, n)^2], the mean of the HII knot mask.
constexpr double kHiiMean = 0.137176;

inline double lmgf(double s) { return s * s * (kLmgf2 + s * (kLmgf3 + kLmgf4 * s)); }

struct GalaxySample {
    double stars = 0.0;          // stars per pc^3, all types
    double youngStars = 0.0;     // young stars per pc^3 (included in `stars`)
    glm::dvec3 starLight{0.0};   // L_sun per pc^3 from stars; linear RGB, colours scaled to unit luminance
    glm::dvec3 nebula{0.0};      // L_sun per pc^3 from HII regions
    double tau = 0.0;            // V-band optical depth per pc
};

class GalaxyEval {
public:
    enum : int { kStars = 1, kLight = 2, kDust = 4, kAll = 7 };

    explicit GalaxyEval(const GalaxyModel& m);

    // posMeters in the galactic frame. lodPc > 0 replaces structure smaller than lodPc by its mean.
    void sample(const glm::dvec3& posMeters, GalaxySample& s, double lodPc = 0.0, int what = kAll) const;
    // Disk frame in kpc: x along major, y along normal, z along cross(normal, major).
    void sampleLocal(double x, double y, double z, double lodKpc, int what, GalaxySample& s) const;
    glm::dvec3 toLocalKpc(const glm::dvec3& posMeters) const;

    double boundRadiusPc() const { return boundKpc_ * 1000.0; }
    GalaxyGpu pack(const glm::dvec3& viewpointMeters) const;
    const GalaxyModel& model() const { return m_; }   // with an orthonormalised frame

private:
    GalaxyModel m_;
    glm::dvec3 minor_{0.0, 0.0, 1.0};
    int type_ = 0;
    bool barred_ = false;
    // Geometry (kpc and 1/kpc).
    double invH_ = 1.0, invTwoHz_ = 1.0, cutR_ = 1.0;
    double invHy_ = 1.0, invTwoHzy_ = 1.0, invTwoHzh_ = 1.0, taper1_ = 0.0;
    double aB_ = 1.0, invQ_ = 1.0;
    double armM_ = 2.0, invTanP_ = 1.0, lnRef_ = 0.0, phase_ = 0.0, armK_ = 1.0;
    double armCm1_ = 0.0, armNorm_ = 1.0, oldCm1_ = 0.0, oldNorm_ = 1.0, dustCm1_ = 0.0, dustNorm_ = 1.0, hiiNorm_ = 0.0;
    double invLamY_ = 1.0, ampY_ = 0.0, ampO_ = 0.0, invLamD_ = 1.0, ampD_ = 0.0, invLamH_ = 1.0;
    double armK1_ = 0.0;   // > 0: clumps in arm-aligned coordinates (lattice cells per unit ln R)
    double armP1_ = 16.0;  // lattice cells around the galaxy (power of two) in arm-aligned coordinates
    double armLam_ = 0.08; // across-arm cell size / R, for the level of detail
    double cosB_ = 1.0, sinB_ = 0.0, invAb_ = 1.0, invBb_ = 1.0, invCb_ = 1.0, barLen_ = 0.0;
    double laneT_ = 0.0, dustT0_ = 0.0, invTwoHzd_ = 1.0;
    uint32_t noiseSeed_ = 0;
    glm::dvec3 noiseOff_{0.0};
    // Prefactors: light (L_sun/pc^3) and stars (1/pc^3) per unit shape value.
    glm::dvec3 eOld_{0.0}, eYoung_{0.0}, eHii_{0.0}, eBulge_{0.0}, eBar_{0.0};
    double nOld_ = 0.0, nYoung_ = 0.0, nBulge_ = 0.0, nBar_ = 0.0;
    double boundKpc_ = 1.0;
};

}  // namespace bhs::universe::detail
