#pragma once
// Physical constants and the geometric units of the black hole.
//
// Frames used throughout the code:
//   * Renderer frame (geometric units): black hole at the origin, +Y = spin axis / accretion disk
//     normal, lengths in M = G M_bh / c^2 and times in G M_bh / c^3. Camera, planets of the black
//     hole system, the accretion disk and the star-disruption simulation live here.
//   * Galactic frame (SI): origin at the centre of the galaxy that hosts the black hole, +Y = host
//     galaxy disk normal (galactic north), meters, double precision. Stars, galaxies and solar
//     system bodies of the 3D universe live here. See universe/Universe.h for the mapping.
#include <string>

namespace bhs::units {

constexpr double kG = 6.67430e-11;                 // m^3 kg^-1 s^-2
constexpr double kC = 299792458.0;                 // m/s
constexpr double kSolarMass = 1.98847e30;          // kg
constexpr double kSolarRadius = 6.957e8;           // m
constexpr double kSolarLuminosity = 3.828e26;      // W
constexpr double kSolarAbsMagV = 4.83;
constexpr double kEarthRadius = 6.3710e6;          // m
constexpr double kEarthMass = 5.9722e24;           // kg
constexpr double kAU = 1.495978707e11;             // m
constexpr double kLightYear = 9.4607304725808e15;  // m
constexpr double kParsec = 3.0856775814913673e16;  // m
constexpr double kDay = 86400.0;                   // s
constexpr double kYear = 365.25 * kDay;            // Julian year, s

// Length and time of one geometric unit for a black hole of `massSolar` solar masses.
inline double metersPerM(double massSolar) { return kG * massSolar * kSolarMass / (kC * kC); }
inline double secondsPerM(double massSolar) { return metersPerM(massSolar) / kC; }

// Human-readable formatting ("12.3 km", "4.56 AU", "7.8 ly", "3.2 h", "0.45 c", "4.3 million suns").
std::string formatDistance(double meters);
std::string formatDuration(double seconds);
std::string formatSpeed(double metersPerSecond);
std::string formatMassSolar(double massSolar);
std::string formatFactor(double x);   // "1.41x", "61000x"

}  // namespace bhs::units
