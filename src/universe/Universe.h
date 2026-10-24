#pragma once
// Content of the 3D universe: the host galaxy, explicit stars, other galaxies and planets with
// orbits. Two providers fill it: buildRealUniverse (catalogues of real stars and galaxies, the
// Milky Way, the solar system) and buildInventedUniverse (deterministic, seed-based).
//
// All positions are in the galactic frame: meters, double precision, origin at the centre of the
// host galaxy, +Y = host galaxy disk normal (galactic north). For the real universe the axes are
// galactocentric: +X points from the Sun towards the Galactic Centre, +Y towards the north galactic
// pole, +Z = X x Y (so the Sun orbits towards -Z). The Sun sits at about (-8178 pc, +20.8 pc, 0).
//
// The renderer works in the black hole's frame (see Units.h). The mapping is
//   renderer = R * (galactic - blackHolePos) / metersPerM,
// where R is the rotation that takes `blackHoleSpinAxis` to +Y (built by the renderer).
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "universe/GalaxyModel.h"

namespace bhs::universe {

enum class Mode : int32_t { Backdrop = 0, Invented = 1, Real = 2 };
enum class BlackHolePlacement : int32_t { GalacticCenter = 0, NearSun = 1 };

// Interstellar extinction: StarRecord::absMag is intrinsic (dust-free) with respect to
// UniverseContent::host's dust; catalogue providers remove the host column between the observer and
// each star. The renderer dims every star by the host's dust between the camera and the star (the sky
// bake records the optical depth at fixed distances along every direction).
struct StarRecord {
    glm::dvec3 pos{0.0};        // meters, galactic frame
    float absMag = 4.83f;       // absolute visual magnitude, intrinsic (see above)
    float teff = 5772.0f;       // effective temperature, K
    float radiusSolar = 1.0f;   // photosphere radius in solar radii
    int32_t name = -1;          // index into UniverseContent::names, -1 if unnamed
};

struct GalaxyRecord {
    GalaxyModel model;          // placement, orientation and structure
    float absMag = -20.0f;      // integrated absolute visual magnitude
    int32_t name = -1;
};

enum class PlanetLook : int32_t {
    Rocky = 0, Desert = 1, Ocean = 2, Ice = 3, Earthlike = 4, GasGiantWarm = 5, GasGiantCold = 6, Cloudy = 7, Lava = 8
};

// Keplerian elements in the style of JPL's "Approximate Positions of the Planets" tables:
// semi-major axis (AU), eccentricity, inclination, mean longitude, longitude of perihelion and
// longitude of the ascending node (degrees) at J2000, each with a rate per Julian century.
struct OrbitElements {
    double aAU = 1, e = 0, iDeg = 0, LDeg = 0, varpiDeg = 0, OmegaDeg = 0;
    double aRate = 0, eRate = 0, iRate = 0, LRate = 0, varpiRate = 0, OmegaRate = 0;
};

struct OrbitingBody {
    std::string name;
    int32_t hostStar = -1;              // index into UniverseContent::stars (e.g. the Sun)
    OrbitElements elements;             // relative to the host star, ecliptic J2000 frame
    glm::dmat3 orbitFrame{1.0};         // rotates ecliptic J2000 vectors into the galactic frame
    double radiusKm = 6371.0;
    double massEarth = 1.0;
    PlanetLook look = PlanetLook::Rocky;
    glm::vec3 tint{1.0f};               // colour bias for the procedural look
    bool rings = false;
    double ringInnerKm = 0.0, ringOuterKm = 0.0;
};

struct NamedPlace {                     // extra go-to targets (e.g. "Galactic Centre", "Virgo Cluster")
    std::string name;
    glm::dvec3 pos{0.0};                // meters, galactic frame
    double radiusMeters = 0.0;          // used to choose a viewing distance
};

struct UniverseContent {
    Mode mode = Mode::Backdrop;
    GalaxyModel host;                           // galaxy that contains the black hole, centred at 0
    glm::dvec3 blackHolePos{0.0};               // meters, galactic frame
    glm::dvec3 blackHoleSpinAxis{0.0, 1.0, 0.0};// unit, galactic frame; becomes the renderer's +Y
    double suggestedMassSolar = 0.0;            // e.g. Sagittarius A*; 0 = keep the user's choice
    std::string blackHoleName = "Gargantua";
    std::vector<StarRecord> stars;              // explicit stars (catalogue, or notable invented ones)
    std::vector<GalaxyRecord> galaxies;         // every galaxy except the host
    std::vector<OrbitingBody> bodies;           // planets orbiting entries of `stars`
    std::vector<NamedPlace> places;
    std::vector<std::string> names;
    // Procedural stars are suppressed within `catalogRadiusPc` of `catalogCenter` for stars that
    // the catalogue already contains (brighter than `catalogLimitAbsMag` at that distance).
    glm::dvec3 catalogCenter{0.0};
    double catalogRadiusPc = 0.0;
    float catalogLimitAppMag = 0.0f;            // apparent (observed, dimmed) V completeness seen from catalogCenter
};

// Real universe from the files in `assetDir` (see tools/fetch_real_catalogs.py and assets/real/).
// Returns false and fills `error` if the catalogue files are missing or malformed.
bool buildRealUniverse(UniverseContent& out, const std::string& assetDir, BlackHolePlacement placement,
                       std::string* error);

// Invented universe; the same seed always produces the same universe.
void buildInventedUniverse(UniverseContent& out, uint32_t seed);

}  // namespace bhs::universe
