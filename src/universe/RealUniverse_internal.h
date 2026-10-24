#pragma once
// Internals of the real universe (RealUniverse.cpp), shared with tests/real_universe_tests.cpp:
// coordinate frames, Kepler orbits and the binary catalogue formats written by
// tools/fetch_real_catalogs.py (documented in assets/real/README.md).
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "universe/Universe.h"

namespace bhs::universe::real {

// ------------------------------------------------------------------------------ frames
// Galactocentric frame of UniverseContent (see Universe.h): origin at Sagittarius A*, +X from the
// Sun towards Sgr A*, +Y towards the north galactic pole, +Z = X x Y. Built like astropy's
// Galactocentric frame: x' points at Sgr A*, z' is the IAU north galactic pole orthogonalised
// against x', then the frame is tilted about y' so that the Sun lies kZ0Pc above the plane.
constexpr double kR0Pc = 8178.0;               // Sun - Sgr A* distance, GRAVITY Collaboration 2019 (A&A 625, L10)
constexpr double kZ0Pc = 20.8;                 // Sun above the midplane, Bennett & Bovy 2019 (MNRAS 482, 1417)
constexpr double kSgrARaDeg = 266.41683708;    // Sgr A*, Reid & Brunthaler 2004 (ApJ 616, 872): 17h45m40.0409s
constexpr double kSgrADecDeg = -29.00781056;   //                                                 -29d00m28.118s
constexpr double kObliquityJ2000Deg = 23.43928; // mean obliquity of the ecliptic at J2000 (JPL approximate elements)

glm::dvec3 icrsUnit(double raDeg, double decDeg);
void icrsToRaDec(const glm::dvec3& dir, double& raDeg, double& decDeg);
// Rows: galactic x (l=0, b=0), y (l=90), z (north galactic pole) in ICRS (Hipparcos, ESA 1997).
const glm::dmat3& icrsToGalacticIau();
void galacticLB(const glm::dvec3& icrsDir, double& lDeg, double& bDeg);
// Rotation from ICRS directions to galactocentric directions (inverse = transpose).
const glm::dmat3& icrsToGalactocentric();
glm::dvec3 sunGalactocentricPc();
// Heliocentric ICRS position (parsecs) -> galactocentric position (meters).
glm::dvec3 icrsPcToGalactocentricMeters(const glm::dvec3& heliocentricIcrsPc);
// Ecliptic J2000 -> ICRS (rotation about x by the obliquity) and -> galactocentric (orbitFrame).
glm::dmat3 eclipticToIcrs();
glm::dmat3 eclipticToGalactocentric();

// Velocity frames and distances used for galaxies (same formulas as the fetch script).
double comovingDistanceMpc(double velocityKms);   // flat Lambda-CDM, H0 = 70, Omega_m = 0.3

// ------------------------------------------------------------------------------ orbits
// Heliocentric position (AU, ecliptic J2000) from JPL approximate Keplerian elements at
// `julianCenturies` = (JD_TDB - 2451545.0) / 36525. Newton iteration on Kepler's equation.
glm::dvec3 keplerEclipticAU(const OrbitElements& el, double julianCenturies);
// Sidereal period from the mean-longitude rate (years).
double orbitalPeriodYears(const OrbitElements& el);

// ------------------------------------------------------------------------------ files
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kHeaderBytes = 64;
inline constexpr char kStarMagic[8] = {'B', 'H', 'S', 'S', 'T', 'A', 'R', 'S'};
inline constexpr char kGalaxyMagic[8] = {'B', 'H', 'S', 'G', 'A', 'L', 'X', 'S'};

// Star flags.
constexpr uint16_t kStarDistanceEstimated = 1;   // no usable parallax: spectroscopic distance
constexpr uint16_t kStarTeffFromSpectrum = 2;    // no B-V
constexpr uint16_t kStarTeffHotCorrection = 4;   // O/B/WR: spectral-type Teff above the B-V value
constexpr uint16_t kStarCompanion = 8;           // secondary component of a multiple system
constexpr uint16_t kStarLiteratureDistance = 16;
constexpr uint16_t kStarProperName = 32;         // name is a proper name (not a designation)

// Galaxy flags.
constexpr uint16_t kGalBarred = 1 << 0, kGalWeakBar = 1 << 1, kGalPaMeasured = 1 << 2, kGalHasCz = 1 << 3,
                   kGalMcConnachie = 1 << 4, kGalUngc = 1 << 5, kGal2mrs = 1 << 6, kGalRc3 = 1 << 7,
                   kGalDwarfSpheroidal = 1 << 8, kGalTypeGuessed = 1 << 9, kGalClusterMember = 1 << 10;
enum class DistanceMethod : uint8_t { HubbleFlow = 0, Direct = 1, ClusterMember = 2, Photometric = 3, Literature = 4 };

struct StarFileHeader {
    char magic[8];
    uint32_t version, headerBytes, count, recordBytes, nameCount, nameBytes, payloadCrc, sunIndex;
    float limitAppMag, radiusPc;
    uint32_t sourceVersion, reserved[3];
};
struct StarFileRecord {
    float x, y, z;              // heliocentric ICRS, parsecs
    float absMag;               // V - 5 log10(d / 10 pc): includes the extinction towards the Sun
    float teff, radiusSolar;
    int32_t name;
    uint32_t hip;
    uint16_t flags, reserved;
};
struct GalaxyFileHeader {
    char magic[8];
    uint32_t version, headerBytes, count, recordBytes, nameCount, nameBytes, payloadCrc;
    float hubbleConstant, omegaM;
    uint32_t reserved[5];
};
struct GalaxyFileRecord {
    float x, y, z;              // heliocentric ICRS, Mpc
    float diameterKpc;          // optical D25-equivalent major diameter
    float effRadiusKpc;         // half-light radius where measured, else 0
    float axisRatio;            // observed b/a
    float paDeg;                // major-axis position angle, north through east (random if not measured)
    float inclinationDeg;       // 0 = face-on
    float absMagV, log10StellarMass, czKms, ttype;
    int32_t name;
    uint32_t seed;
    uint16_t flags;
    uint8_t morph;              // GalaxyType
    uint8_t distanceMethod;     // DistanceMethod
};
static_assert(sizeof(StarFileHeader) == 64 && sizeof(GalaxyFileHeader) == 64);
static_assert(sizeof(StarFileRecord) == 36 && sizeof(GalaxyFileRecord) == 60);

struct StarFile {
    StarFileHeader header{};
    std::vector<StarFileRecord> records;
    std::vector<std::string> names;
};
struct GalaxyFile {
    GalaxyFileHeader header{};
    std::vector<GalaxyFileRecord> records;
    std::vector<std::string> names;
};

uint32_t crc32(const void* data, size_t bytes);
bool parseStarFile(const std::vector<unsigned char>& bytes, StarFile& out, std::string& error);
bool parseGalaxyFile(const std::vector<unsigned char>& bytes, GalaxyFile& out, std::string& error);

// ------------------------------------------------------------------------------ content
// Directory that holds stars.bin and galaxies.bin: `assetDir` itself or `assetDir`/real.
std::string resolveCatalogDir(const std::string& assetDir);
GalaxyModel milkyWayModel();
GalaxyModel galaxyModelFromRecord(const GalaxyFileRecord& rec, const std::string& name);

// Catalogue magnitudes include the real interstellar extinction towards the Sun, but the renderer
// dims every star by the host galaxy model's dust between the camera and the star. The loader
// therefore de-reddens each catalogue star with the model's own column from the Sun:
//     StarRecord::absMag = V - 5 log10(d / 10 pc) - hostDustColumnMag(host, Sun, star),
// so that a renderer that applies the same column (camera -> star) reproduces the catalogue V from
// the Sun and dims or brightens stars consistently elsewhere. Intrinsic magnitudes are capped at
// kBrightestStarAbsMag where the model's dust is far thicker than the real dust in that direction.
constexpr double kDustStepPc = 20.0;            // midpoint-rule step; 30-40 pc steps miss model cloud cores
constexpr double kDustSlabScaleHeights = 10.0;  // the column is integrated where |y| < 10 dust scale heights
constexpr float kBrightestStarAbsMag = -10.0f;  // no known star is intrinsically brighter in V
// V-band extinction (magnitudes) of host.dustExtinction() along the straight segment from -> to
// (meters, galactic frame).
double hostDustColumnMag(const GalaxyModel& host, const glm::dvec3& from, const glm::dvec3& to);
// Disk normals of these galaxies are oriented so that the model's arms wind on our sky as in images
// (+1: counter-clockwise outward on a north-up, east-left image; -1: clockwise); 0 if not listed.
int armWindingOverride(const std::string& galaxyName);
void addSolarSystem(UniverseContent& content, int32_t sunIndex);
// Fictional "Gargantua" placement: unit direction (galactocentric axes) and distance from the Sun.
// RA/Dec were chosen with the HYG v4.4 data (assets/real/README.md): towards Antlia/Hydra
// (l = 268.3, b = +27.4), 23.4 deg from the nearest star brighter than V = 3, and 2.1 pc from the
// nearest catalogue star at 30 light years.
constexpr double kGargantuaRaDeg = 158.682;
constexpr double kGargantuaDecDeg = -26.137;
constexpr double kGargantuaDistanceLy = 30.0;
glm::dvec3 gargantuaDirection();

struct LoadReport {
    double seconds = 0.0;
    size_t starsSkipped = 0, galaxiesSkipped = 0;
    // De-reddening of the catalogue stars (see hostDustColumnMag).
    double dustSeconds = 0.0;
    double medianExtinction = 0.0, maxExtinction = 0.0;   // A_V from the Sun, magnitudes
    size_t starsAtBrightestLimit = 0;                      // capped at kBrightestStarAbsMag
};
const LoadReport& lastLoadReport();

}  // namespace bhs::universe::real
