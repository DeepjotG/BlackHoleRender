// The real universe: catalogue stars (HYG v4.4), galaxies (McConnachie 2012, UNGC, 2MRS), the Milky
// Way as host galaxy, the solar system and named places, all in the galactocentric frame of
// Universe.h. Catalogue files are produced by tools/fetch_real_catalogs.py (see assets/real/README.md).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <system_error>
#include <thread>
#include <utility>

#include "Log.h"
#include "Units.h"
#include "universe/RealUniverse_internal.h"

namespace bhs::universe {

namespace real {
namespace {

static_assert(std::endian::native == std::endian::little, "the catalogue files are little-endian");

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
constexpr double kH0 = 70.0;          // km/s/Mpc, as in the fetch script
constexpr double kOmegaM = 0.3;
constexpr double kCkms = 299792.458;
constexpr double kDiskHalfLightPerH = 1.678;   // half-light radius of an exponential disk / scale length
constexpr double kIrregularQ0 = 0.35;          // intrinsic thickness used for irregulars' inclinations

glm::dmat3 fromRows(const glm::dvec3& r0, const glm::dvec3& r1, const glm::dvec3& r2) {
    return glm::transpose(glm::dmat3(r0, r1, r2));
}

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

// Deterministic pseudo-random number in [0, 1) for a galaxy seed and a purpose ("salt").
double hash01(uint32_t seed, uint32_t salt) {
    return mix32(seed ^ mix32(salt * 0x9E3779B9u + 0x7F4A7C15u)) / 4294967296.0;
}

bool finite(float v) { return std::isfinite(v); }

LoadReport g_report;

}  // namespace

// ------------------------------------------------------------------------------------- frames

glm::dvec3 icrsUnit(double raDeg, double decDeg) {
    const double ra = raDeg * kDeg, dec = decDeg * kDeg;
    return {std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
}

void icrsToRaDec(const glm::dvec3& dir, double& raDeg, double& decDeg) {
    const glm::dvec3 d = glm::normalize(dir);
    raDeg = std::atan2(d.y, d.x) / kDeg;
    if (raDeg < 0.0) raDeg += 360.0;
    decDeg = std::asin(std::clamp(d.z, -1.0, 1.0)) / kDeg;
}

const glm::dmat3& icrsToGalacticIau() {
    // IAU 1958 galactic system realised in the ICRS (Hipparcos catalogue, ESA 1997, vol. 1, 1.5.3).
    static const glm::dmat3 m = fromRows({-0.0548755604162154, -0.8734370902348850, -0.4838350155487132},
                                         {+0.4941094278755837, -0.4448296299600112, +0.7469822444972189},
                                         {-0.8676661490190047, -0.1980763734312015, +0.4559837761750669});
    return m;
}

void galacticLB(const glm::dvec3& icrsDir, double& lDeg, double& bDeg) {
    const glm::dvec3 g = icrsToGalacticIau() * glm::normalize(icrsDir);
    lDeg = std::atan2(g.y, g.x) / kDeg;
    if (lDeg < 0.0) lDeg += 360.0;
    bDeg = std::asin(std::clamp(g.z, -1.0, 1.0)) / kDeg;
}

const glm::dmat3& icrsToGalactocentric() {
    static const glm::dmat3 m = [] {
        const glm::dvec3 xp = icrsUnit(kSgrARaDeg, kSgrADecDeg);               // towards Sgr A*
        const glm::dvec3 ngp = glm::dvec3(glm::transpose(icrsToGalacticIau())[2]);  // IAU north galactic pole
        const glm::dvec3 zp = glm::normalize(ngp - glm::dot(ngp, xp) * xp);
        const glm::dvec3 yp = glm::cross(zp, xp);
        const glm::dmat3 b = fromRows(xp, yp, zp);
        // Tilt about y' so that the Sun (at x' = -R0) ends up z0 above the plane.
        const double th = std::asin(kZ0Pc / kR0Pc);
        const glm::dmat3 h = fromRows({std::cos(th), 0.0, std::sin(th)}, {0.0, 1.0, 0.0},
                                      {-std::sin(th), 0.0, std::cos(th)});
        // (x'', y'', z'') -> (X, Y, Z) = (x'', z'', -y''): +Y north galactic pole, +Z = X x Y.
        const glm::dmat3 p = fromRows({1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, -1.0, 0.0});
        return p * h * b;
    }();
    return m;
}

glm::dvec3 sunGalactocentricPc() {
    const double th = std::asin(kZ0Pc / kR0Pc);
    return {-kR0Pc * std::cos(th), kR0Pc * std::sin(th), 0.0};
}

glm::dvec3 icrsPcToGalactocentricMeters(const glm::dvec3& p) {
    const glm::dvec3 gc = kR0Pc * icrsUnit(kSgrARaDeg, kSgrADecDeg);
    return icrsToGalactocentric() * (p - gc) * units::kParsec;
}

glm::dmat3 eclipticToIcrs() {
    const double e = kObliquityJ2000Deg * kDeg;
    return fromRows({1.0, 0.0, 0.0}, {0.0, std::cos(e), -std::sin(e)}, {0.0, std::sin(e), std::cos(e)});
}

glm::dmat3 eclipticToGalactocentric() { return icrsToGalactocentric() * eclipticToIcrs(); }

double comovingDistanceMpc(double v) {
    const double z = std::clamp(v, 0.0, 0.6 * kCkms) / kCkms;
    int n = std::max(16, static_cast<int>(z * 4000.0));
    n += n & 1;                             // composite Simpson needs an even number of intervals
    const auto e = [](double zz) { return std::sqrt(kOmegaM * (1 + zz) * (1 + zz) * (1 + zz) + (1 - kOmegaM)); };
    double s = 0.0;                         // Simpson's rule
    for (int i = 0; i <= n; ++i) {
        const double w = (i == 0 || i == n) ? 1.0 : (i % 2 ? 4.0 : 2.0);
        s += w / e(z * i / n);
    }
    return s * (z / n) / 3.0 * kCkms / kH0;
}

namespace {

// Same frame conversions as the fetch script (Local Group / CMB frames, blended at 2500-4500 km/s).
double flowVelocity(double vHelio, const glm::dvec3& icrsDir) {
    double l, b;
    galacticLB(icrsDir, l, b);
    l *= kDeg;
    b *= kDeg;
    const double vlg = vHelio + 316.0 * (std::sin(b) * std::sin(-4.0 * kDeg) +
                                         std::cos(b) * std::cos(-4.0 * kDeg) * std::cos(l - 93.0 * kDeg));
    const double la = 264.021 * kDeg, ba = 48.253 * kDeg;
    const double vcmb = vHelio + 369.82 * (std::sin(b) * std::sin(ba) + std::cos(b) * std::cos(ba) * std::cos(l - la));
    const double w = std::clamp((vlg - 2500.0) / 2000.0, 0.0, 1.0);
    return (1.0 - w) * vlg + w * vcmb;
}

glm::dvec3 placeFromRaDec(double raDeg, double decDeg, double distPc) {
    return icrsPcToGalactocentricMeters(distPc * icrsUnit(raDeg, decDeg));
}

}  // namespace

// ------------------------------------------------------------------------------------- orbits

glm::dvec3 keplerEclipticAU(const OrbitElements& el, double t) {
    // E. M. Standish, "Keplerian Elements for Approximate Positions of the Major Planets" (JPL).
    const double a = el.aAU + el.aRate * t;
    const double e = std::clamp(el.e + el.eRate * t, 0.0, 0.99);
    const double inc = (el.iDeg + el.iRate * t) * kDeg;
    const double meanLon = el.LDeg + el.LRate * t;
    const double varpi = el.varpiDeg + el.varpiRate * t;
    const double node = (el.OmegaDeg + el.OmegaRate * t) * kDeg;
    const double omega = varpi * kDeg - node;
    double m = std::fmod(meanLon - varpi, 360.0);
    if (m > 180.0) m -= 360.0;
    if (m < -180.0) m += 360.0;
    m *= kDeg;
    double ecc = m + e * std::sin(m);
    for (int k = 0; k < 50; ++k) {
        const double d = (m - (ecc - e * std::sin(ecc))) / (1.0 - e * std::cos(ecc));
        ecc += d;
        if (std::fabs(d) < 1e-13) break;
    }
    const double xp = a * (std::cos(ecc) - e);
    const double yp = a * std::sqrt(1.0 - e * e) * std::sin(ecc);
    const double co = std::cos(omega), so = std::sin(omega), cn = std::cos(node), sn = std::sin(node);
    const double ci = std::cos(inc), si = std::sin(inc);
    return {(co * cn - so * sn * ci) * xp + (-so * cn - co * sn * ci) * yp,
            (co * sn + so * cn * ci) * xp + (-so * sn + co * cn * ci) * yp,
            (so * si) * xp + (co * si) * yp};
}

double orbitalPeriodYears(const OrbitElements& el) {
    return el.LRate > 0.0 ? 36000.0 / el.LRate : 0.0;   // 360 deg / (LRate per century) in years
}

// ------------------------------------------------------------------------------------- files

uint32_t crc32(const void* data, size_t bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace {

template <class Header>
bool checkHeader(const std::vector<unsigned char>& bytes, const char (&magic)[8], uint32_t recordBytes,
                 Header& h, uint64_t& expectedSize, std::string& error) {
    if (bytes.size() < kHeaderBytes) {
        error = "file is shorter than its header";
        return false;
    }
    std::memcpy(&h, bytes.data(), sizeof h);
    if (std::memcmp(h.magic, magic, 8) != 0) {
        error = "wrong file type (bad magic)";
        return false;
    }
    if (h.version != kFormatVersion) {
        error = "unsupported format version " + std::to_string(h.version) + " (expected " +
                std::to_string(kFormatVersion) + "); run tools/fetch_real_catalogs.py";
        return false;
    }
    if (h.headerBytes != kHeaderBytes || h.recordBytes != recordBytes) {
        error = "unexpected header or record size";
        return false;
    }
    if (h.count > 20'000'000u || h.nameCount > h.count + 1u || h.nameBytes > (1u << 28)) {
        error = "implausible record or name counts";
        return false;
    }
    expectedSize = uint64_t(kHeaderBytes) + uint64_t(h.count) * recordBytes + (uint64_t(h.nameCount) + 1) * 4 +
                   h.nameBytes;
    if (bytes.size() != expectedSize) {
        error = "file size " + std::to_string(bytes.size()) + " does not match its header (" +
                std::to_string(expectedSize) + "); truncated or corrupt";
        return false;
    }
    if (crc32(bytes.data() + kHeaderBytes, bytes.size() - kHeaderBytes) != h.payloadCrc) {
        error = "checksum mismatch; the file is corrupt";
        return false;
    }
    return true;
}

bool parseNames(const std::vector<unsigned char>& bytes, size_t offset, uint32_t count, uint32_t blobBytes,
                std::vector<std::string>& names, std::string& error) {
    std::vector<uint32_t> offs(size_t(count) + 1);
    std::memcpy(offs.data(), bytes.data() + offset, offs.size() * 4);
    const char* blob = reinterpret_cast<const char*>(bytes.data() + offset + offs.size() * 4);
    if (offs.front() != 0 || offs.back() != blobBytes) {
        error = "corrupt name table";
        return false;
    }
    // Validate every offset before touching the blob: non-decreasing and inside it.
    for (uint32_t i = 0; i < count; ++i) {
        if (offs[i + 1] < offs[i] || offs[i + 1] > blobBytes) {
            error = "corrupt name table offsets";
            return false;
        }
    }
    names.clear();
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        names.emplace_back(blob + offs[i], offs[i + 1] - offs[i]);
        if (names.back().find('\0') != std::string::npos) {
            error = "corrupt name table (NUL)";
            return false;
        }
    }
    return true;
}

bool readFile(const std::filesystem::path& path, std::vector<unsigned char>& bytes, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot read " + path.string() + " (" + ec.message() + ")";
        return false;
    }
    if (size > (uint64_t(1) << 31)) {
        error = path.string() + " is implausibly large";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    bytes.resize(static_cast<size_t>(size));
    if (!f || !f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

}  // namespace

bool parseStarFile(const std::vector<unsigned char>& bytes, StarFile& out, std::string& error) {
    uint64_t size = 0;
    if (!checkHeader(bytes, kStarMagic, sizeof(StarFileRecord), out.header, size, error)) return false;
    const StarFileHeader& h = out.header;
    if (h.count == 0 || h.sunIndex >= h.count) {
        error = "no stars or invalid Sun index";
        return false;
    }
    if (!std::isfinite(h.limitAppMag) || !std::isfinite(h.radiusPc) || h.radiusPc < 0.0f) {
        error = "invalid completeness fields";
        return false;
    }
    out.records.resize(h.count);
    std::memcpy(out.records.data(), bytes.data() + kHeaderBytes, size_t(h.count) * sizeof(StarFileRecord));
    return parseNames(bytes, kHeaderBytes + size_t(h.count) * sizeof(StarFileRecord), h.nameCount, h.nameBytes,
                      out.names, error);
}

bool parseGalaxyFile(const std::vector<unsigned char>& bytes, GalaxyFile& out, std::string& error) {
    uint64_t size = 0;
    if (!checkHeader(bytes, kGalaxyMagic, sizeof(GalaxyFileRecord), out.header, size, error)) return false;
    const GalaxyFileHeader& h = out.header;
    out.records.resize(h.count);
    if (h.count) {
        std::memcpy(out.records.data(), bytes.data() + kHeaderBytes, size_t(h.count) * sizeof(GalaxyFileRecord));
    }
    return parseNames(bytes, kHeaderBytes + size_t(h.count) * sizeof(GalaxyFileRecord), h.nameCount, h.nameBytes,
                      out.names, error);
}

std::string resolveCatalogDir(const std::string& assetDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base(assetDir.empty() ? std::string(".") : assetDir);
    for (const fs::path& d : {base, base / "real"}) {
        if (fs::is_regular_file(d / "stars.bin", ec)) return d.string();   // galaxies.bin is optional
    }
    return {};
}

const LoadReport& lastLoadReport() { return g_report; }

// ------------------------------------------------------------------------------------- galaxies

GalaxyModel milkyWayModel() {
    GalaxyModel m;
    m.type = GalaxyType::BarredSpiral;
    m.seed = 0x4D696C6Bu;
    m.center = glm::dvec3(0.0);
    m.normal = glm::dvec3(0.0, 1.0, 0.0);        // north galactic pole; the disk rotates clockwise seen from +Y
    m.major = glm::dvec3(1.0, 0.0, 0.0);         // azimuth 0 points away from the Sun (the Sun is at 180 deg)
    m.stellarMassSolar = 5.0e10;                 // (5 +- 1) x 10^10, Bland-Hawthorn & Gerhard 2016 (ARA&A 54, 529)
    m.luminositySolar = 3.0e10;                  // V band, M/L_V ~ 1.7 (Licquia et al. 2015 colours; BH&G 2016)
    m.diskScaleLengthPc = 2600.0;                // thin disk, 2.6 +- 0.5 kpc (BH&G 2016)
    m.diskScaleHeightPc = 300.0;                 // thin disk, 300 +- 50 pc (BH&G 2016)
    m.diskCutoffPc = 15000.0;                    // stellar disk edge near 14 kpc (Minniti et al. 2011, ApJ 733, L43)
    m.bulgeMassFraction = 0.25;                  // bulge/bar ~1.4-1.7e10 Msun (Portail et al. 2015; BH&G 2016)
    m.bulgeRadiusPc = 800.0;
    m.bulgeFlattening = 0.6;                     // boxy/peanut bulge (Wegg & Gerhard 2013, MNRAS 435, 1874)
    // Long bar: half-length 5.0 +- 0.2 kpc (Wegg, Gerhard & Portail 2015, MNRAS 450, 4050); 4.8 kpc puts
    // its ends on the inner ends of the model arms. Bar angle 27 deg from the Sun-centre line with the
    // near end at positive longitude (Wegg & Gerhard 2013): azimuth 180 - 27 deg in this frame.
    m.barLengthPc = 4800.0;
    m.barAxisRatio = 0.35;
    m.barAngleDeg = 180.0 - 27.0;
    // Arms: one logarithmic pattern with four arms of pitch 12 deg (Vallee 2015, 2017; Reid et al. 2014,
    // ApJ 783, 130 find 7-20 deg per arm). The phase is a least-squares fit, in ln R at the Sun's azimuth,
    // to the Sagittarius-Carina (6.8 kpc), Perseus (10.1 kpc) and Outer (13.8 kpc) arms of Reid et al.
    // (2014), rescaled to R0 = 8.178 kpc: arms then cross the Sun-centre line at 5.05 (Scutum-Centaurus),
    // 7.05 (Sagittarius-Carina), 9.84 (Perseus) and 13.7 kpc (Norma-Outer). GalaxyModel has no per-arm
    // strength, so the two major (Scutum-Centaurus, Perseus) and two minor arms are equally strong.
    m.armCount = 4;
    m.armPitchDeg = 12.0;
    m.armRefRadiusPc = 7050.0;
    m.armPhaseDeg = 0.0;
    m.armWidth = 0.4;
    m.armContrast = 3.0;
    // Dust: scale height 0.0134 R0 ~ 110 pc (Drimmel & Spergel 2001, ApJ 556, 181). A_V = 5 mag/kpc at
    // R = 2.6 kpc gives a mean of ~0.6 mag/kpc near the Sun for an exponential dust disk with the stellar
    // scale length (observed: ~0.7 mag/kpc). A plain exponential would put ~34 mag in the midplane
    // between the Sun and Sgr A* (observed A_V ~ 30-40); GalaxyModel thins the dust inside the bar
    // (apart from the bar's dust lanes), which gives ~14 mag: the centre stays invisible in V.
    m.dustScaleHeightPc = 110.0;
    m.dustExtinctionPerKpc = 5.0;
    return m;
}

int armWindingOverride(const std::string& galaxyName) {
    // Sense in which the arms wind outward on north-up, east-left images (+1 counter-clockwise, -1
    // clockwise). Measured on DSS2 (blue, red) and GALEX NUV cutouts (CDS hips2fits): after
    // deprojection, the phase of the m = 2, 3, 4 azimuthal harmonics rotates monotonically with
    // log radius, consistently across surveys and harmonics, and checked by eye on face-on renderings
    // (assets/real/README.md). M31 is too inclined for that: its south-west half approaches (Chemin
    // et al. 2009, ApJ 705, 1395), its north-west side is near, and its arms trail, so it rotates
    // counter-clockwise on the sky and its arms wind clockwise outward. M33's images were ambiguous.
    static const std::pair<const char*, int> kWinding[] = {
        {"Andromeda Galaxy (M31)", -1}, {"Whirlpool Galaxy (M51)", -1},         {"Bode's Galaxy (M81)", -1},
        {"Pinwheel Galaxy (M101)", -1}, {"Southern Pinwheel Galaxy (M83)", +1}, {"Phantom Galaxy (M74)", +1},
    };
    for (const auto& [n, w] : kWinding) {
        if (galaxyName == n) return w;
    }
    return 0;
}

double hostDustColumnMag(const GalaxyModel& host, const glm::dvec3& from, const glm::dvec3& to) {
    if (!(host.dustExtinctionPerKpc > 0.0)) return 0.0;
    const glm::dvec3 seg = to - from;
    const double lenPc = glm::length(seg) / units::kParsec;
    if (!(lenPc > 0.0) || !std::isfinite(lenPc)) return 0.0;
    // Only the host's dust matters: clip the segment to its bounding sphere (GalaxyModel.h: dust is
    // negligible outside) and to |y| <= kDustSlabScaleHeights dust scale heights around its plane
    // (sech^2 profile: 2e-4 of the midplane density there). This also bounds the work per star.
    double t0 = 0.0, t1 = 1.0;
    const double rb = host.boundingRadiusPc() * units::kParsec;
    if (rb > 0.0 && std::isfinite(rb)) {
        const glm::dvec3 rel = from - host.center;
        const double qa = glm::dot(seg, seg), qb = 2.0 * glm::dot(rel, seg), qc = glm::dot(rel, rel) - rb * rb;
        const double disc = qb * qb - 4.0 * qa * qc;
        if (!(disc > 0.0)) return 0.0;
        const double sq = std::sqrt(disc);
        t0 = std::max(t0, (-qb - sq) / (2.0 * qa));
        t1 = std::min(t1, (-qb + sq) / (2.0 * qa));
        if (!(t1 > t0)) return 0.0;
    }
    const double nLen = glm::length(host.normal);
    if (host.dustScaleHeightPc > 0.0 && std::isfinite(host.dustScaleHeightPc) && nLen > 0.0) {
        const glm::dvec3 n = host.normal / nLen;
        const double y = kDustSlabScaleHeights * host.dustScaleHeightPc * units::kParsec;
        const double ya = glm::dot(from - host.center, n), yb = glm::dot(to - host.center, n);
        if ((ya > y && yb > y) || (ya < -y && yb < -y)) return 0.0;
        if (yb != ya) {
            double ta = (y - ya) / (yb - ya), tb = (-y - ya) / (yb - ya);
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (!(t1 > t0)) return 0.0;
        }
    }
    const double pathPc = (t1 - t0) * lenPc;
    const int n = std::clamp(static_cast<int>(std::ceil(pathPc / kDustStepPc)), 1, 1 << 16);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double a = host.dustExtinction(from + seg * (t0 + (t1 - t0) * ((i + 0.5) / n)));   // A_V per pc
        if (a > 0.0 && std::isfinite(a)) sum += a;
    }
    return sum * pathPc / n;
}

namespace {

// De-reddens the catalogue stars with the host's dust column from the Sun (see hostDustColumnMag in
// RealUniverse_internal.h). About 1.9 million dust evaluations: spread over the hardware threads.
void dereddenStars(UniverseContent& c, int32_t sunIndex, const glm::dvec3& sunPos, LoadReport& report) {
    const auto t0 = std::chrono::steady_clock::now();
    const size_t count = c.stars.size();
    std::vector<double> column(count, 0.0);
    constexpr size_t kChunk = 256;
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (;;) {
            const size_t begin = next.fetch_add(kChunk, std::memory_order_relaxed);
            if (begin >= count) return;
            for (size_t i = begin; i < std::min(count, begin + kChunk); ++i) {
                if (static_cast<int32_t>(i) != sunIndex) column[i] = hostDustColumnMag(c.host, sunPos, c.stars[i].pos);
            }
        }
    };
    const unsigned threads = count < 16 * kChunk ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < threads; ++k) {
        try {
            pool.emplace_back(work);
        } catch (const std::system_error&) {
            break;   // fewer threads; the calling thread does the rest
        }
    }
    work();
    for (std::thread& t : pool) t.join();

    std::vector<double> nonSun;
    nonSun.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (static_cast<int32_t>(i) == sunIndex) continue;
        StarRecord& s = c.stars[i];
        const float intrinsic = static_cast<float>(s.absMag - column[i]);
        s.absMag = std::max(intrinsic, kBrightestStarAbsMag);
        report.starsAtBrightestLimit += intrinsic < kBrightestStarAbsMag;
        report.maxExtinction = std::max(report.maxExtinction, column[i]);
        nonSun.push_back(column[i]);
    }
    if (!nonSun.empty()) {
        std::nth_element(nonSun.begin(), nonSun.begin() + nonSun.size() / 2, nonSun.end());
        report.medianExtinction = nonSun[nonSun.size() / 2];
    }
    report.dustSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

double interp(const std::initializer_list<std::pair<double, double>>& pts, double x) {
    const auto* p = pts.begin();
    if (x <= p->first) return p->second;
    for (const auto* q = p + 1; q != pts.end(); ++q, ++p) {
        if (x <= q->first) return p->second + (q->second - p->second) * (x - p->first) / (q->first - p->first);
    }
    return (pts.end() - 1)->second;
}

}  // namespace

GalaxyModel galaxyModelFromRecord(const GalaxyFileRecord& r, const std::string& name) {
    GalaxyModel m;
    m.type = static_cast<GalaxyType>(std::min<uint8_t>(r.morph, 4));
    m.seed = r.seed ? r.seed : 1u;
    const glm::dvec3 posPc = glm::dvec3(r.x, r.y, r.z) * 1e6;
    m.center = icrsPcToGalactocentricMeters(posPc);
    const uint32_t s = m.seed;
    const double t = r.ttype;
    const double q = std::clamp(static_cast<double>(r.axisRatio), 0.05, 1.0);
    const double r25 = std::max(0.5 * r.diameterKpc * 1000.0, 10.0);   // pc
    // Ultra-faint dwarfs (Segue 1, Willman 1) have a few hundred solar masses of stars.
    m.stellarMassSolar = std::pow(10.0, std::clamp(static_cast<double>(r.log10StellarMass), 2.0, 13.0));
    m.luminositySolar = std::pow(10.0, -0.4 * (std::clamp(static_cast<double>(r.absMagV), -30.0, 5.0) - units::kSolarAbsMagV));

    // Orientation on the sky: major axis at the position angle (north through east), the disk normal
    // inclined from the line of sight towards the minor axis. Which side is nearer (nearSign) and which
    // face of the disk we see (upSign: it sets the sense in which the arms wind on the sky and the
    // sense of rotation) are not catalogued. Both come from the seed, except: the near sides of M31
    // and the LMC, and the winding of a few famous spirals (armWindingOverride).
    const glm::dvec3 los = glm::normalize(posPc);
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), los);
    east = glm::length(east) < 1e-9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::normalize(east);
    const glm::dvec3 north = glm::cross(los, east);
    const double pa = r.paDeg * kDeg;
    const glm::dvec3 majorSky = std::cos(pa) * north + std::sin(pa) * east;
    const glm::dvec3 minorSky = -std::sin(pa) * north + std::cos(pa) * east;
    double incl = std::clamp(static_cast<double>(r.inclinationDeg), 0.0, 90.0) * kDeg;
    if (m.type == GalaxyType::Elliptical) incl = 0.5 * kPi;   // oblate, seen edge-on: projected = intrinsic shape
    double nearSign = hash01(s, 21) < 0.5 ? 1.0 : -1.0;       // +1: the side at PA - 90 deg is nearer
    // +1: +normal faces us, so the arms (counter-clockwise outward seen from +normal, GalaxyModel.h)
    // wind counter-clockwise outward on a north-up, east-left image of our sky.
    double upSign = hash01(s, 22) < 0.5 ? 1.0 : -1.0;
    if (name == "Andromeda Galaxy (M31)") nearSign = 1.0;     // PA 35 deg: north-west (PA 305) is near
    if (name == "Large Magellanic Cloud") nearSign = 1.0;     // PA 170 deg: the north-east side is near
                                                              // (van der Marel & Cioni 2001, AJ 122, 1807)
    if (const int w = armWindingOverride(name)) upSign = w;
    const glm::dvec3 nIcrs = upSign * (std::cos(incl) * -los + nearSign * std::sin(incl) * minorSky);
    const glm::dmat3& rot = icrsToGalactocentric();
    m.normal = glm::normalize(rot * nIcrs);
    glm::dvec3 major = rot * majorSky;
    major = glm::normalize(major - glm::dot(major, m.normal) * m.normal);
    m.major = major;

    const double ha = hash01(s, 1), hb = hash01(s, 2), hc = hash01(s, 3), hd = hash01(s, 4);
    if (m.type == GalaxyType::Elliptical) {
        const bool dsph = (r.flags & kGalDwarfSpheroidal) != 0;
        // Effective radius: measured (Local Group dwarfs) or R25 / 2.5 for an r^(1/4) profile.
        const double re = std::clamp(r.effRadiusKpc > 0.0f ? r.effRadiusKpc * 1000.0 : r25 / 2.5, 10.0, 2.0e5);
        const double qe = std::clamp(q, dsph ? 0.2 : 0.3, 1.0);
        m.ellipticalRadiusPc = re;
        m.ellipticalAxisRatio = qe;
        m.diskScaleLengthPc = re;
        m.diskScaleHeightPc = re * qe;
        m.diskCutoffPc = 8.0 * re;
        m.bulgeMassFraction = 1.0;
        m.bulgeRadiusPc = re;
        m.bulgeFlattening = qe;
        m.barLengthPc = 0.0;
        m.armCount = 2;
        m.armContrast = 1.0;
        m.dustExtinctionPerKpc = 0.0;
        m.dustScaleHeightPc = 0.1 * re;
        const bool giant = r.log10StellarMass > 10.0f;
        m.bulgeColor = giant ? glm::vec3(1.0f, 0.78f, 0.56f) : glm::vec3(1.0f, 0.84f, 0.66f);
        m.diskColor = m.bulgeColor;
        return m;
    }

    // Disks. R25 is about four scale lengths for normal spirals (Freeman disks reach 25 mag/arcsec^2
    // at ~3-4.5 h), fewer for low-surface-brightness irregulars.
    const double rPerH = m.type == GalaxyType::Irregular ? 2.8 : (m.type == GalaxyType::Lenticular ? 3.5 : 4.0);
    const double h = std::clamp(r25 / rPerH, 30.0, 3.0e4);
    m.diskScaleLengthPc = h;
    // The "ellipticals and irregulars" fields (GalaxyModel.h). Effective radius: measured (Local Group
    // dwarfs) or the half-light radius of an exponential disk, 1.678 h. Axis ratio: for irregulars the
    // intrinsic flattening along `normal` that matches the observed b/a at the inclination the fetch
    // script derived with q0 = 0.35 (min(b/a, 0.35)); for lenticulars and spirals the observed b/a,
    // which is what a sprite of the galaxy should show.
    m.ellipticalRadiusPc = r.effRadiusKpc > 0.0f ? std::clamp(r.effRadiusKpc * 1000.0, 10.0, 2.0e5) : kDiskHalfLightPerH * h;
    m.ellipticalAxisRatio = m.type == GalaxyType::Irregular ? std::clamp(std::min(q, kIrregularQ0), 0.1, 1.0) : std::clamp(q, 0.1, 1.0);
    m.diskScaleHeightPc = h * interp({{-3, 0.22}, {0, 0.18}, {1, 0.15}, {3, 0.12}, {5, 0.11}, {7, 0.14}, {9, 0.22}, {10, 0.30}}, t);
    m.diskCutoffPc = std::max(1.25 * r25, 4.0 * h);
    // Bulge-to-total by type (K band, e.g. Graham & Worley 2008).
    m.bulgeMassFraction = m.type == GalaxyType::Irregular
                              ? 0.0
                              : interp({{-3, 0.45}, {0, 0.40}, {1, 0.30}, {2, 0.25}, {3, 0.18}, {4, 0.12}, {5, 0.08}, {6, 0.05}, {7, 0.03}, {8, 0.01}, {9, 0.0}}, t);
    m.bulgeRadiusPc = h * (0.15 + 0.4 * m.bulgeMassFraction);
    m.bulgeFlattening = t < 1.5 ? 0.7 : 0.6;
    // Bars: semi-major axis ~1-1.5 h in early types, shorter in late types (Erwin 2005); in barred
    // Magellanic spirals (the LMC) the bar is the dominant, long feature.
    const bool barred = (r.flags & kGalBarred) != 0;
    const bool magellanic = t >= 8.5;
    if (barred && m.type != GalaxyType::Irregular) {
        double lb = magellanic ? 1.4 * h : h * interp({{0, 1.3}, {3, 1.2}, {6, 0.9}, {8, 0.8}}, t);
        if (r.flags & kGalWeakBar) lb *= 0.6;
        m.barLengthPc = lb;
        m.barAxisRatio = 0.3 + 0.15 * ha;
        m.barAngleDeg = 360.0 * hb;
    } else {
        m.barLengthPc = 0.0;
    }
    // Arms: grand design (two arms) in early and barred types, more and looser arms in late types;
    // pitch angle grows with type (Kennicutt 1981; Ma 2002).
    if (m.type == GalaxyType::Lenticular) {
        m.armCount = 2;
        m.armContrast = 1.0;
    } else if (m.type == GalaxyType::Irregular) {
        m.armCount = 2;
        m.armPitchDeg = 25.0;
        m.armContrast = 1.4;
    } else if (magellanic) {
        m.armCount = 1;                 // Magellanic spirals have a single arm (de Vaucouleurs & Freeman 1972)
        m.armPitchDeg = 25.0;
        m.armContrast = 1.8;
    } else {
        m.armCount = (barred || t <= 4.0 || hc < 0.7) ? 2 : (hc < 0.85 ? 3 : 4);
        m.armPitchDeg = interp({{1, 8.0}, {3, 12.0}, {5, 17.0}, {7, 22.0}, {9, 25.0}}, t) + 4.0 * (hd - 0.5);
        m.armContrast = interp({{1, 2.0}, {3, 3.0}, {5, 3.0}, {7, 2.2}, {9, 1.6}}, t);
    }
    m.armWidth = 0.35 + 0.1 * hash01(s, 5);
    if (m.barLengthPc > 0.0) {         // arms start at the ends of the bar
        m.armRefRadiusPc = m.barLengthPc;
        m.armPhaseDeg = m.barAngleDeg;
    } else {
        m.armRefRadiusPc = 1.5 * h;
        m.armPhaseDeg = 360.0 * hash01(s, 6);
    }
    // Dust: face-on A_V at R = h by type, spread over a layer 0.4 x the stellar scale height.
    const double faceOnAv = m.type == GalaxyType::Irregular
                                ? 0.25
                                : interp({{-3, 0.1}, {0, 0.2}, {1, 0.6}, {3, 0.9}, {5, 0.8}, {7, 0.4}, {9, 0.25}}, t);
    m.dustScaleHeightPc = 0.4 * m.diskScaleHeightPc;
    m.dustExtinctionPerKpc = faceOnAv / (2.0 * m.dustScaleHeightPc / 1000.0);
    // Colours: early types redder.
    if (t < 1.5) {
        m.diskColor = t < 0.5 ? glm::vec3(1.0f, 0.85f, 0.68f) : glm::vec3(0.98f, 0.88f, 0.78f);
    } else if (t > 7.5) {
        m.diskColor = glm::vec3(0.85f, 0.90f, 1.0f);
    }
    return m;
}

// ------------------------------------------------------------------------------------- solar system

void addSolarSystem(UniverseContent& c, int32_t sunIndex) {
    // Elements and rates per Julian century, ecliptic and equinox of J2000, valid 1800-2050:
    // E. M. Standish, JPL "Keplerian Elements for Approximate Positions of the Major Planets", Table 1
    // (https://ssd.jpl.nasa.gov/planets/approx_pos.html; Pluto from the same table's earlier text
    // version, ssd.jpl.nasa.gov/txt/p_elem_t1.txt). Earth uses the Earth-Moon barycentre.
    // Radii (mean) and masses: NASA GSFC Planetary Fact Sheet; Pluto radius from New Horizons.
    struct Planet {
        const char* name;
        OrbitElements el;
        double radiusKm, massEarth;
        PlanetLook look;
        glm::vec3 tint;
        bool rings;
        double ringInner, ringOuter;
    };
    static const Planet planets[] = {
        {"Mercury",
         {0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,
          0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081},
         2439.7, 0.0553, PlanetLook::Rocky, {0.78f, 0.75f, 0.72f}, false, 0.0, 0.0},
        {"Venus",
         {0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,
          0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418},
         6051.8, 0.815, PlanetLook::Cloudy, {1.0f, 0.93f, 0.76f}, false, 0.0, 0.0},
        {"Earth",
         {1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
          0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0},
         6371.0, 1.0, PlanetLook::Earthlike, {1.0f, 1.0f, 1.0f}, false, 0.0, 0.0},
        {"Mars",
         {1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,
          0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343},
         3389.5, 0.107, PlanetLook::Desert, {1.0f, 0.62f, 0.42f}, false, 0.0, 0.0},
        {"Jupiter",
         {5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
          -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106},
         69911.0, 317.83, PlanetLook::GasGiantWarm, {1.0f, 0.88f, 0.72f}, false, 0.0, 0.0},
        {"Saturn",
         {9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,
          -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794},
         58232.0, 95.16, PlanetLook::GasGiantWarm, {1.0f, 0.92f, 0.70f}, true, 74500.0, 140220.0},
        {"Uranus",
         {19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,
          -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589},
         25362.0, 14.54, PlanetLook::GasGiantCold, {0.72f, 0.92f, 0.96f}, false, 0.0, 0.0},
        {"Neptune",
         {30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,
          0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664},
         24622.0, 17.15, PlanetLook::GasGiantCold, {0.45f, 0.60f, 1.0f}, false, 0.0, 0.0},
        {"Pluto",
         {39.48211675, 0.24882730, 17.14001206, 238.92903833, 224.06891629, 110.30393684,
          -0.00031596, 0.00005170, 0.00004818, 145.20780515, -0.04062942, -0.01183482},
         1188.3, 0.00218, PlanetLook::Ice, {0.95f, 0.83f, 0.70f}, false, 0.0, 0.0},
    };
    const glm::dmat3 frame = eclipticToGalactocentric();
    for (const Planet& p : planets) {
        OrbitingBody b;
        b.name = p.name;
        b.hostStar = sunIndex;
        b.elements = p.el;
        b.orbitFrame = frame;
        b.radiusKm = p.radiusKm;
        b.massEarth = p.massEarth;
        b.look = p.look;
        b.tint = p.tint;
        b.rings = p.rings;
        b.ringInnerKm = p.ringInner;
        b.ringOuterKm = p.ringOuter;
        c.bodies.push_back(std::move(b));
    }
}

// ------------------------------------------------------------------------------------- placement

glm::dvec3 gargantuaDirection() {
    // Chosen once from the catalogue (see assets/real/README.md): among directions at moderate galactic
    // latitude (10-35 deg, the Milky Way's band nearby), the one farthest from every star brighter
    // than V = 3 with no catalogue star within 2 pc of the point 30 light years out.
    static const glm::dvec3 d = glm::normalize(icrsToGalactocentric() * icrsUnit(kGargantuaRaDeg, kGargantuaDecDeg));
    return d;
}

namespace {

// Spin axis tilted `tiltDeg` from galactic north, oriented so that the disk is seen at
// `inclinationDeg` from `viewer` (a unit vector from the hole towards the viewer) when possible.
glm::dvec3 tiltedSpin(double tiltDeg, double inclinationDeg, const glm::dvec3& towardsViewer) {
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const double ct = std::cos(tiltDeg * kDeg), st = std::sin(tiltDeg * kDeg);
    glm::dvec3 hz = towardsViewer - glm::dot(towardsViewer, up) * up;
    const double hl = glm::length(hz);
    if (hl < 1e-9) return glm::normalize(ct * up + st * glm::dvec3(1.0, 0.0, 0.0));
    hz /= hl;
    const double c = std::clamp((std::cos(inclinationDeg * kDeg) - ct * glm::dot(towardsViewer, up)) / (st * hl), -1.0, 1.0);
    const glm::dvec3 side = glm::cross(up, hz);
    const glm::dvec3 h = c * hz + std::sqrt(1.0 - c * c) * side;
    return glm::normalize(ct * up + st * h);
}

void addPlaces(UniverseContent& c) {
    const double pc = units::kParsec, mpc = 1e6 * units::kParsec;
    const auto findStar = [&](const char* name) -> int {
        for (size_t i = 0; i < c.stars.size(); ++i) {
            const int32_t n = c.stars[i].name;
            if (n >= 0 && c.names[n] == name) return static_cast<int>(i);
        }
        return -1;
    };
    const auto centroid = [&](std::initializer_list<const char*> members, double ra, double dec, double distPc) {
        glm::dvec3 sum(0.0);
        int n = 0;
        for (const char* m : members) {
            const int i = findStar(m);
            if (i >= 0) {
                sum += c.stars[i].pos;
                ++n;
            }
        }
        // Use the catalogue members when most are present so the place matches the rendered stars.
        return n >= static_cast<int>(members.size()) / 2 + 1 ? sum / double(n) : placeFromRaDec(ra, dec, distPc);
    };
    const auto findGalaxy = [&](const char* name) -> const GalaxyRecord* {
        for (const GalaxyRecord& g : c.galaxies) {
            if (g.name >= 0 && c.names[g.name] == name) return &g;
        }
        return nullptr;
    };
    const auto cluster = [&](double ra, double dec, double vHelio) {
        const glm::dvec3 u = icrsUnit(ra, dec);
        return comovingDistanceMpc(flowVelocity(vHelio, u)) * 1e6;   // pc
    };
    const glm::dvec3 sun = sunGalactocentricPc() * pc;
    std::vector<NamedPlace>& p = c.places;
    p.push_back({"Galactic Centre (Sagittarius A*)", glm::dvec3(0.0), 100.0 * pc});
    p.push_back({"Solar System", sun, 50.0 * units::kAU});
    p.push_back({"Orion Nebula (M42)", placeFromRaDec(83.8221, -5.3911, 388.0), 4.0 * pc});  // Kounkel et al. 2017
    p.push_back({"Pleiades (M45)",
                 centroid({"Alcyone", "Atlas", "Electra", "Maia", "Merope", "Taygeta", "Pleione", "Celaeno", "Asterope"},
                          56.75, 24.1167, 136.0),
                 5.0 * pc});
    p.push_back({"Hyades", centroid({"Prima Hyadum", "Secunda Hyadum", "Ain", "Chamukuy"}, 66.75, 15.87, 47.5), 6.0 * pc});
    p.push_back({"Crab Nebula (M1)", placeFromRaDec(83.6331, 22.0145, 2000.0), 2.0 * pc});       // Trimble 1973
    p.push_back({"Eagle Nebula (M16)", placeFromRaDec(274.7000, -13.8167, 1740.0), 10.0 * pc});  // Kuhn et al. 2019
    p.push_back({"Carina Nebula", placeFromRaDec(161.2650, -59.8667, 2350.0), 20.0 * pc});        // Smith 2006
    p.push_back({"Ring Nebula (M57)", placeFromRaDec(283.3963, 33.0291, 790.0), 0.5 * pc});       // Gaia parallax
    // Globular clusters: distances from Baumgardt & Vasiliev 2021 (MNRAS 505, 5957).
    p.push_back({"Omega Centauri", placeFromRaDec(201.6968, -47.4795, 5430.0), 20.0 * pc});
    p.push_back({"47 Tucanae", placeFromRaDec(6.0236, -72.0813, 4520.0), 15.0 * pc});
    p.push_back({"Hercules Globular Cluster (M13)", placeFromRaDec(250.4235, 36.4613, 7420.0), 15.0 * pc});
    // Local Group barycentre taken half way between the Milky Way and M31 (McConnachie 2012).
    const GalaxyRecord* m31 = findGalaxy("Andromeda Galaxy (M31)");
    const glm::dvec3 m31Pos = m31 ? m31->model.center : placeFromRaDec(10.6847, 41.2690, 761.0e3);
    p.push_back({"Local Group", 0.5 * m31Pos, 1.0 * mpc});
    // Clusters at the distances used for their member galaxies by the fetch script.
    p.push_back({"Virgo Cluster", placeFromRaDec(187.70593, 12.39112, 16.5e6), 2.2 * mpc});    // Mei et al. 2007
    p.push_back({"Fornax Cluster", placeFromRaDec(54.62108, -35.45074, 20.0e6), 1.0 * mpc});   // Blakeslee et al. 2009
    p.push_back({"Coma Cluster", placeFromRaDec(194.9531, 27.9807, cluster(194.9531, 27.9807, 6925.0)), 3.0 * mpc});
    p.push_back({"Perseus Cluster", placeFromRaDec(49.95067, 41.51170, cluster(49.95067, 41.51170, 5366.0)), 2.5 * mpc});
    p.push_back({"Centaurus Cluster", placeFromRaDec(192.20521, -41.31111, cluster(192.20521, -41.31111, 3400.0)), 2.0 * mpc});
    p.push_back({"Hydra Cluster", placeFromRaDec(159.17821, -27.52817, cluster(159.17821, -27.52817, 3777.0)), 2.0 * mpc});
    // The Great Attractor: the Norma cluster (Abell 3627) at the core of the overdensity
    // (Kraan-Korteweg et al. 1996; Woudt et al. 2008).
    p.push_back({"Great Attractor (Norma Cluster)", placeFromRaDec(243.7658, -60.9072, cluster(243.7658, -60.9072, 4871.0)),
                 15.0 * mpc});
    p.push_back({"Shapley Supercluster", placeFromRaDec(201.9870, -31.4955, cluster(201.9870, -31.4955, 14390.0)), 30.0 * mpc});
}

}  // namespace
}  // namespace real

// ------------------------------------------------------------------------------------- entry point

bool buildRealUniverse(UniverseContent& out, const std::string& assetDir, BlackHolePlacement placement,
                       std::string* error) {
    using namespace real;
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    const auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        logError("real universe: {}", msg);
        return false;
    };
    const std::string dir = resolveCatalogDir(assetDir);
    if (dir.empty()) {
        return fail("catalogue file stars.bin not found in '" + assetDir + "' or '" + assetDir +
                    "/real'; run tools/fetch_real_catalogs.py");
    }
    std::vector<unsigned char> bytes;
    StarFile sf;
    GalaxyFile gf;
    if (!readFile(std::filesystem::path(dir) / "stars.bin", bytes, err) || !parseStarFile(bytes, sf, err)) {
        return fail("stars.bin: " + err);
    }
    // galaxies.bin is derived from AAS-copyrighted tables and is not distributed with the code: when it
    // has not been generated locally (tools/fetch_real_catalogs.py), the universe has no other galaxies.
    std::error_code gec;
    if (std::filesystem::is_regular_file(std::filesystem::path(dir) / "galaxies.bin", gec)) {
        if (!readFile(std::filesystem::path(dir) / "galaxies.bin", bytes, err) || !parseGalaxyFile(bytes, gf, err)) {
            return fail("galaxies.bin: " + err);
        }
    } else {
        logWarn("real universe: galaxies.bin not found; run tools/fetch_real_catalogs.py to add the real galaxies");
    }
    std::vector<unsigned char>().swap(bytes);

    UniverseContent c;
    c.mode = Mode::Real;
    c.host = milkyWayModel();
    c.names.reserve(sf.names.size() + gf.names.size());
    for (std::string& n : sf.names) c.names.push_back(std::move(n));
    const int32_t galaxyNameBase = static_cast<int32_t>(c.names.size());
    for (std::string& n : gf.names) c.names.push_back(std::move(n));
    const int32_t starNames = galaxyNameBase, galaxyNames = static_cast<int32_t>(gf.names.size());

    // Stars.
    LoadReport report;
    const glm::dvec3 sunPos = sunGalactocentricPc() * units::kParsec;
    int32_t sunIndex = -1;
    c.stars.reserve(sf.records.size());
    for (uint32_t i = 0; i < sf.records.size(); ++i) {
        const StarFileRecord& r = sf.records[i];
        const bool sun = i == sf.header.sunIndex;
        // Catalogue stars belong to the Milky Way: anything farther than 100 kpc is corrupt.
        const bool ok = finite(r.x) && finite(r.y) && finite(r.z) && finite(r.absMag) && finite(r.teff) &&
                        finite(r.radiusSolar) && std::fabs(r.x) < 1e5f && std::fabs(r.y) < 1e5f &&
                        std::fabs(r.z) < 1e5f && r.absMag > -20.0f && r.absMag < 30.0f && r.teff >= 500.0f &&
                        r.teff <= 3.0e5f && r.radiusSolar > 0.0f && r.radiusSolar < 1e5f && r.name >= -1 &&
                        r.name < starNames;
        if (!ok) {
            if (sun) return fail("stars.bin: invalid Sun record");
            ++report.starsSkipped;
            continue;
        }
        StarRecord s;
        if (sun) {
            sunIndex = static_cast<int32_t>(c.stars.size());
            s.pos = sunPos;
            s.absMag = static_cast<float>(units::kSolarAbsMagV);
            s.teff = 5772.0f;
            s.radiusSolar = 1.0f;
            s.name = r.name;
            if (s.name < 0 || c.names[s.name] != "Sun") {
                s.name = static_cast<int32_t>(c.names.size());
                c.names.push_back("Sun");
            }
        } else {
            s.pos = icrsPcToGalactocentricMeters(glm::dvec3(r.x, r.y, r.z));
            s.absMag = r.absMag;
            s.teff = r.teff;
            s.radiusSolar = r.radiusSolar;
            s.name = r.name;
        }
        c.stars.push_back(s);
    }
    dereddenStars(c, sunIndex, sunPos, report);

    // Galaxies.
    c.galaxies.reserve(gf.records.size());
    for (const GalaxyFileRecord& r : gf.records) {
        const bool ok = finite(r.x) && finite(r.y) && finite(r.z) && finite(r.diameterKpc) && finite(r.axisRatio) &&
                        finite(r.paDeg) && finite(r.inclinationDeg) && finite(r.absMagV) &&
                        finite(r.log10StellarMass) && finite(r.ttype) && finite(r.effRadiusKpc) &&
                        std::fabs(r.x) < 2e4f && std::fabs(r.y) < 2e4f && std::fabs(r.z) < 2e4f &&
                        (r.x != 0.0f || r.y != 0.0f || r.z != 0.0f) && r.diameterKpc > 0.0f &&
                        r.diameterKpc < 1e4f && r.axisRatio > 0.0f && r.axisRatio <= 1.0f &&
                        r.inclinationDeg >= 0.0f && r.inclinationDeg <= 90.0f && r.absMagV > -30.0f &&
                        r.absMagV < 5.0f && r.morph <= 4 && r.name >= -1 && r.name < galaxyNames;
        if (!ok) {
            ++report.galaxiesSkipped;
            continue;
        }
        GalaxyRecord g;
        const std::string& name = r.name >= 0 ? c.names[galaxyNameBase + r.name] : std::string();
        g.model = galaxyModelFromRecord(r, name);
        g.absMag = r.absMagV;
        g.name = r.name >= 0 ? galaxyNameBase + r.name : -1;
        c.galaxies.push_back(g);
    }

    addSolarSystem(c, sunIndex);
    addPlaces(c);

    // Procedural stars duplicate the catalogue within this sphere around the Sun for stars brighter
    // than the catalogue's completeness (HYG: Hipparcos Survey complete to V ~ 7.3-9; the radius is the
    // 99th percentile distance of catalogue stars brighter than the limit, written by the fetch script).
    c.catalogCenter = sunPos;
    c.catalogRadiusPc = sf.header.radiusPc;
    c.catalogLimitAppMag = sf.header.limitAppMag;

    if (placement == BlackHolePlacement::GalacticCenter) {
        c.blackHolePos = glm::dvec3(0.0);
        c.suggestedMassSolar = 4.297e6;        // GRAVITY Collaboration 2022 (A&A 657, L12)
        c.blackHoleName = "Sagittarius A*";
        // The true spin direction is unknown (EHT 2022 favours a viewing inclination below ~50 deg).
        // Artistic choice: 50 deg from galactic north, leaning towards the Sun (rotated 20 deg in
        // azimuth), so the Milky Way's plane crosses the accretion disk at a steep angle.
        const glm::dvec3 towardsSun = glm::normalize(glm::dvec3(-1.0, 0.0, 0.0));
        const double az = 20.0 * kDeg;
        const glm::dvec3 h(towardsSun.x * std::cos(az), 0.0, towardsSun.x * std::sin(az));
        c.blackHoleSpinAxis = glm::normalize(std::cos(50.0 * kDeg) * glm::dvec3(0.0, 1.0, 0.0) + std::sin(50.0 * kDeg) * h);
    } else {
        const glm::dvec3 towardsHole = gargantuaDirection();
        c.blackHolePos = sunPos + towardsHole * (kGargantuaDistanceLy * units::kLightYear);
        c.suggestedMassSolar = 1.0e8;          // Gargantua in "Interstellar" (Thorne 2014)
        c.blackHoleName = "Gargantua";
        // Tilted 40 deg from galactic north (the Milky Way's band crosses the disk diagonally) and
        // seen almost edge-on (80 deg, like the film's view) from the Sun.
        c.blackHoleSpinAxis = tiltedSpin(40.0, 80.0, -towardsHole);
        c.places.push_back({"Gargantua", c.blackHolePos, 1.0 * units::kAU});
    }

    out = std::move(c);
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_report = report;
    logInfo("real universe: {} stars, {} galaxies, {} planets, {} places, {} names from {} in {:.0f} ms{}",
            out.stars.size(), out.galaxies.size(), out.bodies.size(), out.places.size(), out.names.size(), dir,
            report.seconds * 1e3,
            report.starsSkipped + report.galaxiesSkipped
                ? std::format(" ({} stars and {} galaxies skipped as invalid)", report.starsSkipped, report.galaxiesSkipped)
                : std::string());
    logInfo("real universe: stars de-reddened by the Milky Way model's dust (median A_V {:.3f}, max {:.1f} mag, "
            "{} at the M_V = {:.0f} limit) in {:.0f} ms",
            report.medianExtinction, report.maxExtinction, report.starsAtBrightestLimit, kBrightestStarAbsMag,
            report.dustSeconds * 1e3);
    return true;
}

}  // namespace bhs::universe
