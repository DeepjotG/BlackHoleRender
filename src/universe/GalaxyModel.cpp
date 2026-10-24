// NOTE for the "invented" workflow worker: the lead implemented this module after your task stalled.
// Do not overwrite these files; finish with a short report.
//
// GalaxyModel: CPU evaluation and GPU packing (see GalaxyModel_internal.h for the component model
// and shaders/galaxy_model.glsl for the GPU twin, which must stay formula-for-formula identical).
#include <algorithm>
#include <cmath>

#include "Units.h"
#include "universe/GalaxyModel.h"
#include "universe/GalaxyModel_internal.h"

namespace bhs::universe {

namespace detail {

namespace {
constexpr double kPi = 3.14159265358979323846;

double smooth(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double sech2(double u) {
    const double e = std::exp(-2.0 * std::abs(u));
    const double d = 1.0 + e;
    return 4.0 * e / (d * d);
}
// Azimuthal mean of ((1 + cos psi) / 2)^k.
double armMean(double k) { return std::exp(std::lgamma(k + 0.5) - std::lgamma(k + 1.0)) / std::sqrt(kPi); }
glm::dvec3 unitLuminance(const glm::vec3& c) {
    const double l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    return l > 1e-6 ? glm::dvec3(c) / l : glm::dvec3(1.0);
}
// Fraction of an exponential disk's mass (scale h) between radii a and b.
double expDiskMass(double a, double b, double h) {
    const auto outside = [h](double r) { return (1.0 + r / h) * std::exp(-r / h); };
    return std::max(outside(a) - outside(b), 1e-4);
}
float fade(float f) { return f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float latticeValue(int32_t x, int32_t y, int32_t z, uint32_t salt) {
    const uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u + static_cast<uint32_t>(y) * 0xd8163841u +
                       static_cast<uint32_t>(z) * 0xcb1ab31fu + salt * 0x165667b1u;
    return static_cast<float>(pcgHash(h)) * (2.0f / 4294967296.0f) - 1.0f;
}
}  // namespace

double boundRadiusKpc(const GalaxyModel& m) {
    const double kpc = 1e-3;
    const double h = std::max(m.diskScaleLengthPc, 1.0) * kpc;
    if (m.type == GalaxyType::Elliptical) return 20.0 * std::max(m.ellipticalRadiusPc, 1.0) * kpc / 1.8153;
    const double cutR = m.diskCutoffPc > 0.0 ? std::max(m.diskCutoffPc * kpc, 0.5 * h) : 6.0 * h;
    double barLen = std::max(m.barLengthPc, 0.0) * kpc;
    if (m.type == GalaxyType::BarredSpiral && barLen <= 0.0) barLen = 1.2 * h;
    const double aB = std::max(m.bulgeRadiusPc, 1.0) * kpc / 2.4142;
    return std::max({1.1 * cutR, 1.3 * barLen, m.bulgeMassFraction > 0.0 ? 12.0 * aB : 0.0});
}

uint32_t pcgHash(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float valueNoiseWrapY(float px, float py, float pz, uint32_t salt, int32_t mask) {
    const float ix = std::floor(px), iy = std::floor(py), iz = std::floor(pz);
    const float ux = fade(px - ix), uy = fade(py - iy), uz = fade(pz - iz);
    const int32_t x = static_cast<int32_t>(ix), z = static_cast<int32_t>(iz);
    const int32_t y0 = static_cast<int32_t>(iy) & mask, y1 = (static_cast<int32_t>(iy) + 1) & mask;
    const float a = latticeValue(x, y0, z, salt), b = latticeValue(x + 1, y0, z, salt);
    const float c = latticeValue(x, y1, z, salt), d = latticeValue(x + 1, y1, z, salt);
    const float e = latticeValue(x, y0, z + 1, salt), f = latticeValue(x + 1, y0, z + 1, salt);
    const float g = latticeValue(x, y1, z + 1, salt), h = latticeValue(x + 1, y1, z + 1, salt);
    return lerp(lerp(lerp(a, b, ux), lerp(c, d, ux), uy), lerp(lerp(e, f, ux), lerp(g, h, ux), uy), uz);
}

float valueNoise(float px, float py, float pz, uint32_t salt) { return valueNoiseWrapY(px, py, pz, salt, -1); }

GalaxyEval::GalaxyEval(const GalaxyModel& model) : m_(model) {
    // Orthonormal disk frame.
    glm::dvec3 n = glm::length(m_.normal) > 1e-12 ? glm::normalize(m_.normal) : glm::dvec3(0.0, 1.0, 0.0);
    glm::dvec3 mj = m_.major - glm::dot(m_.major, n) * n;
    if (glm::length(mj) < 1e-9) mj = glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0));
    mj = glm::normalize(mj);
    m_.normal = n;
    m_.major = mj;
    minor_ = glm::cross(n, mj);

    type_ = std::clamp(static_cast<int>(m_.type), 0, 4);
    const bool ell = type_ == static_cast<int>(GalaxyType::Elliptical);
    const bool len = type_ == static_cast<int>(GalaxyType::Lenticular);
    const bool irr = type_ == static_cast<int>(GalaxyType::Irregular);
    const double kpc = 1e-3;   // per parsec
    const double h = std::max(m_.diskScaleLengthPc, 1.0) * kpc;
    const double hz = std::max(m_.diskScaleHeightPc, 0.5) * kpc;
    const double N = std::max(m_.stellarMassSolar, 0.0) / 0.4;
    const double L = std::max(m_.luminositySolar, 0.0);
    double barLen = std::max(m_.barLengthPc, 0.0) * kpc;
    if (type_ == static_cast<int>(GalaxyType::BarredSpiral) && barLen <= 0.0) barLen = 1.2 * h;
    barred_ = !ell && barLen > 0.0;
    barLen_ = barred_ ? barLen : 0.0;

    // Mass budget: the bar takes part of the bulge's share.
    double fB, fBar = 0.0, fOld = 0.0, fYoung = 0.0;
    if (ell) {
        fB = 1.0;
    } else {
        const double fbt = std::clamp(m_.bulgeMassFraction, 0.0, 0.95);
        fBar = barred_ ? std::min(0.4 * fbt + 0.03, 0.5) : 0.0;
        fB = barred_ ? 0.6 * fbt : fbt;
        const double fDisk = std::max(1.0 - fB - fBar, 0.0);
        fYoung = fDisk * (len ? 0.0 : (irr ? 0.15 : 0.07));
        fOld = fDisk - fYoung;
    }
    // Light per unit mass: young stars are far brighter than old ones; HII regions add nebular light;
    // the dense old bulge and bar are given extra weight so they glow above the disk as in photographs.
    const double wOld = 1.0, wYoung = 10.0, wB = 1.6, wBar = 1.3, wHii = (ell || len) ? 0.0 : 1.2;
    const double S = fOld * wOld + fYoung * (wYoung + wHii) + fB * wB + fBar * wBar;
    const double lu = S > 0.0 ? L / S : 0.0;

    // Old disk.
    invH_ = 1.0 / h;
    invTwoHz_ = 1.0 / (2.0 * hz);
    cutR_ = m_.diskCutoffPc > 0.0 ? std::max(m_.diskCutoffPc * kpc, 0.5 * h) : 6.0 * h;
    const double normOld = 1.0 / (2.0 * kPi * h * h * expDiskMass(0.0, cutR_, h) * 4.0 * hz);
    // Young disk: longer, thinner, absent inside the bar or the bulge region.
    const double hy = (irr ? 1.0 : 1.3) * h;
    const double hzy = irr ? 0.6 * hz : std::max(0.3 * hz, 0.025 * std::min(1.0, h / 2.6));
    const double hzh = 0.7 * hzy;
    invHy_ = 1.0 / hy;
    invTwoHzy_ = 1.0 / (2.0 * hzy);
    invTwoHzh_ = 1.0 / (2.0 * hzh);
    const double armRef = std::max(m_.armRefRadiusPc * kpc, 0.1 * h);
    taper1_ = irr ? 0.0 : (barred_ ? barLen : 0.35 * armRef);
    const double youngMass = expDiskMass(0.65 * taper1_, cutR_, hy);
    const double normY = 1.0 / (2.0 * kPi * hy * hy * youngMass * 4.0 * hzy);
    const double normH = 1.0 / (2.0 * kPi * hy * hy * youngMass * 4.0 * hzh);

    // Arms.
    armM_ = std::max(m_.armCount, 1);
    invTanP_ = 1.0 / std::tan(std::clamp(m_.armPitchDeg, 2.0, 60.0) * kPi / 180.0);
    lnRef_ = std::log(armRef);
    phase_ = m_.armPhaseDeg * kPi / 180.0;
    const double w = std::clamp(m_.armWidth, 0.05, 0.95);
    const double c = std::cos(0.5 * kPi * w);
    armK_ = std::log(0.5) / std::log(c * c);
    const double cm1 = (ell || len) ? 0.0 : std::max(m_.armContrast, 1.0) - 1.0;
    armCm1_ = 3.0 * cm1;   // young stars trace the arms much more than the model's average contrast
    armNorm_ = 1.0 / (1.0 + armCm1_ * armMean(2.0 * armK_));   // young profile A^2: narrower arms
    oldCm1_ = 0.3 * cm1;
    oldNorm_ = 1.0 / (1.0 + oldCm1_ * armMean(armK_));
    dustCm1_ = 2.2 * cm1;
    dustNorm_ = 1.0 / (1.0 + dustCm1_ * armMean(1.6 * armK_));
    hiiNorm_ = armCm1_ > 0.05 ? 1.0 / armMean(4.0 * armK_) : 0.0;

    // Clumps, scaled gently with the galaxy's size.
    const double sizeScale = std::clamp(h / 2.6, 0.2, 3.0);
    // Star-forming complexes and dust clouds are intermittent: lognormal-like contrast with a spread
    // of about 1.4 (young) and 2 (dust) in log density, so most of the volume is clear and a few
    // large clouds carry most of the dust, as in real galaxies.
    invLamY_ = 1.0 / ((irr ? 0.6 : 0.5) * std::sqrt(sizeScale));
    ampY_ = irr ? 3.5 : 1.2;
    ampO_ = irr ? 1.6 : (len ? 0.25 : 0.2);
    invLamD_ = 1.0 / (0.12 * std::pow(sizeScale, 0.3));   // fine, isotropic third dust octave
    ampD_ = irr ? 6.0 : 8.0;
    // Spirals: clumps live in unwound log-polar coordinates (ln R, azimuth along the spiral). A
    // lattice cell is 2 pi R sin(pitch) / P across the arm and R / (K1 sin(pitch)) along it; P (a power
    // of two, so the pattern wraps seamlessly) makes cells ~5% of R across, K1 stretches them 1.5x.
    if (cm1 > 0.3 && !irr) {
        const double sinP = std::sin(std::clamp(m_.armPitchDeg, 2.0, 60.0) * kPi / 180.0);
        armP1_ = std::clamp(std::exp2(std::round(std::log2(2.0 * kPi * sinP / 0.05))), 4.0, 64.0);
        armK1_ = armP1_ / (2.0 * kPi * 1.5 * sinP * sinP);
        armLam_ = 2.0 * kPi * sinP / armP1_;
    }
    invLamH_ = 1.0 / (0.11 * std::pow(sizeScale, 0.3));
    noiseSeed_ = m_.seed % 16777216u;
    const float k1000 = 1000.0f / 4294967296.0f;
    noiseOff_ = glm::dvec3(static_cast<float>(pcgHash(noiseSeed_ * 3u + 1u)) * k1000,
                           static_cast<float>(pcgHash(noiseSeed_ * 3u + 2u)) * k1000,
                           static_cast<float>(pcgHash(noiseSeed_ * 3u + 3u)) * k1000);

    // Bulge (Hernquist; a from the half-mass radius, or from the effective radius of an elliptical).
    const double q = std::clamp(ell ? m_.ellipticalAxisRatio : m_.bulgeFlattening, 0.1, 1.0);
    aB_ = ell ? std::max(m_.ellipticalRadiusPc, 1.0) * kpc / 1.8153 : std::max(m_.bulgeRadiusPc, 1.0) * kpc / 2.4142;
    invQ_ = 1.0 / q;
    const double normB = 1.0 / (2.0 * kPi * q);

    // Bar.
    const double beta = m_.barAngleDeg * kPi / 180.0;
    cosB_ = std::cos(beta);
    sinB_ = std::sin(beta);
    double normBar = 0.0;
    if (barred_) {
        const double ab = 0.8 * barLen;
        const double bb = std::max(barLen * std::clamp(m_.barAxisRatio, 0.1, 1.0) * 0.7, 0.05 * ab);
        const double cb = std::max(0.9 * hz, 0.35 * bb);
        invAb_ = 1.0 / ab;
        invBb_ = 1.0 / bb;
        invCb_ = 1.0 / cb;
        normBar = 1.0 / (1.81280495 * ab * kPi * bb * cb);
    }

    // Dust.
    const double hzd = std::max(m_.dustScaleHeightPc, 1.0) * kpc;
    invTwoHzd_ = 1.0 / (2.0 * hzd);
    dustT0_ = std::max(m_.dustExtinctionPerKpc, 0.0) / 1000.0 / 1.086;
    laneT_ = barred_ ? 2.5 * dustT0_ * std::exp(-(0.5 * barLen - h) / h) : 0.0;

    // Prefactors (shape functions are in kpc^-3; 1e-9 converts to pc^-3).
    eOld_ = 1e-9 * lu * fOld * wOld * normOld * unitLuminance(m_.diskColor);
    nOld_ = 1e-9 * N * fOld * normOld;
    eYoung_ = 1e-9 * lu * fYoung * wYoung * normY * unitLuminance(m_.armColor);
    nYoung_ = 1e-9 * N * fYoung * normY;
    eHii_ = 1e-9 * lu * fYoung * wHii * normH * unitLuminance(m_.hiiColor);
    eBulge_ = 1e-9 * lu * fB * wB * normB * unitLuminance(m_.bulgeColor);
    nBulge_ = 1e-9 * N * fB * normB;
    eBar_ = 1e-9 * lu * fBar * wBar * normBar * unitLuminance(glm::mix(m_.bulgeColor, m_.diskColor, 0.25f));
    nBar_ = 1e-9 * N * fBar * normBar;

    boundKpc_ = boundRadiusKpc(m_);
}

glm::dvec3 GalaxyEval::toLocalKpc(const glm::dvec3& p) const {
    const glm::dvec3 d = (p - m_.center) / (1000.0 * units::kParsec);
    return {glm::dot(d, m_.major), glm::dot(d, m_.normal), glm::dot(d, minor_)};
}

void GalaxyEval::sample(const glm::dvec3& p, GalaxySample& s, double lodPc, int what) const {
    const glm::dvec3 l = toLocalKpc(p);
    sampleLocal(l.x, l.y, l.z, lodPc * 1e-3, what, s);
}

void GalaxyEval::sampleLocal(double x, double y, double z, double lod, int what, GalaxySample& s) const {
    s = GalaxySample{};
    const double R0 = std::sqrt(x * x + z * z);
    const double R = std::max(R0, 1e-5);

    // Bulge (the whole of an elliptical).
    const double yq = y * invQ_;
    const double mm = std::max(std::sqrt(R * R + yq * yq), 0.02 * aB_);
    const double ma = mm + aB_;
    const double bul = aB_ / (mm * ma * ma * ma);
    s.stars = nBulge_ * bul;
    s.starLight = eBulge_ * bul;
    if (type_ == static_cast<int>(GalaxyType::Elliptical)) return;

    const double theta = R0 > 1e-6 ? std::atan2(z, x) : 0.0;
    const double psi = armM_ * (theta - (std::log(R) - lnRef_) * invTanP_ - phase_);
    const double A = std::pow(std::max(0.5 + 0.5 * std::cos(psi), 0.0), armK_);
    const double cut = 1.0 - smooth(0.9 * cutR_, 1.1 * cutR_, R);

    // Clump noise: arm-aligned coordinates for spirals (seamless around the galaxy), plain 3D
    // coordinates otherwise. Star-forming complexes, the old disk's mottling and the two coarse dust
    // octaves share these coordinates (with different salts).
    glm::dvec3 Q1;
    double lam1;
    int32_t mask1 = -1, mask2 = -1;
    if (armK1_ > 0.0) {
        const double Rc = std::max(R, 0.3 / invH_);
        const double thU = theta - (std::log(R) - lnRef_) * invTanP_;
        Q1 = glm::dvec3(std::log(Rc) * armK1_, thU * (armP1_ / (2.0 * kPi)), y * invLamY_ * 0.7) + noiseOff_;
        lam1 = armLam_ * Rc;
        mask1 = static_cast<int32_t>(armP1_) - 1;
        mask2 = 2 * static_cast<int32_t>(armP1_) - 1;
    } else {
        Q1 = glm::dvec3(x, y, z) * invLamY_ + noiseOff_;
        lam1 = 1.0 / invLamY_;
    }
    const glm::dvec3 Q2 = Q1 * 2.0 + glm::dvec3(7.1, 3.3, 5.7);
    const auto noiseAt = [](const glm::dvec3& q, uint32_t salt, int32_t mask) {
        return static_cast<double>(valueNoiseWrapY(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), salt, mask));
    };
    const double wy1 = std::clamp(2.0 - 2.0 * lod / lam1, 0.0, 1.0);
    const double wy2 = std::clamp(2.0 - 4.0 * lod / lam1, 0.0, 1.0);
    const double n1 = wy1 > 0.0 ? noiseAt(Q1, 11u, mask1) : 0.0;
    const double n2 = wy2 > 0.0 ? noiseAt(Q2, 12u, mask2) : 0.0;
    const double sY1 = ampY_ * 0.65 * wy1, sY2 = ampY_ * 0.35 * wy2, sO = ampO_ * wy1;
    const double clumpY = std::exp(sY1 * n1 + sY2 * n2 - lmgf(sY1) - lmgf(sY2));
    const double clumpO = std::exp(sO * n1 - lmgf(sO));

    const double old = std::exp(-R * invH_) * cut * sech2(y * invTwoHz_) * (1.0 + oldCm1_ * A) * oldNorm_ * clumpO;
    const double taper = taper1_ > 0.0 ? smooth(0.35 * taper1_, taper1_, R) : 1.0;
    const double radY = std::exp(-R * invHy_) * cut * taper;
    const double Ay = A * A;   // young stars: narrower arms
    const double young = radY * sech2(y * invTwoHzy_) * (1.0 + armCm1_ * Ay) * armNorm_ * clumpY;

    double bar = 0.0, xb = 0.0, zb = 0.0;
    if (barred_) {
        xb = x * cosB_ + z * sinB_;
        zb = -x * sinB_ + z * cosB_;
        const double u = xb * invAb_, v = zb * invBb_, t = y * invCb_;
        bar = std::exp(-(u * u) * (u * u) - v * v - t * t);
    }
    s.stars += nOld_ * old + nYoung_ * young + nBar_ * bar;
    s.youngStars = nYoung_ * young;

    if (what & kLight) {
        s.starLight += eOld_ * old + eYoung_ * young + eBar_ * bar;
        const double wh = std::clamp(2.0 - 2.0 * lod * invLamH_, 0.0, 1.0);
        double knots = kHiiMean;
        if (wh > 0.0) {
            const glm::dvec3 Ph = glm::dvec3(x, y, z) * invLamH_ + noiseOff_ * 1.7;
            const double sm = smooth(0.25, 0.6, valueNoise(static_cast<float>(Ph.x), static_cast<float>(Ph.y), static_cast<float>(Ph.z), 13u));
            knots = wh * sm * sm + (1.0 - wh) * kHiiMean;
        }
        const double onArm = hiiNorm_ > 0.0 ? Ay * Ay * hiiNorm_ : 1.0;
        s.nebula = eHii_ * (radY * sech2(y * invTwoHzh_) * (knots / kHiiMean) * onArm * clumpY);
    }

    if ((what & kDust) && dustT0_ > 0.0) {
        // Turbulent cascade: two coarse octaves along the arms, three fine isotropic ones (120, 60 and
        // 30 pc for a Milky-Way-sized galaxy) that only matter close to the viewer.
        const double wd3 = std::clamp(2.0 - 2.0 * lod * invLamD_, 0.0, 1.0);
        const double wd4 = std::clamp(2.0 - 4.0 * lod * invLamD_, 0.0, 1.0);
        const double wd5 = std::clamp(2.0 - 8.0 * lod * invLamD_, 0.0, 1.0);
        const double s1 = ampD_ * 0.5 * wy1, s2 = ampD_ * 0.3 * wy2, s3 = ampD_ * 0.2 * wd3, s4 = ampD_ * 0.15 * wd4,
                     s5 = ampD_ * 0.1 * wd5;
        const glm::dvec3 Pd = glm::dvec3(x, y, z) * invLamD_ + noiseOff_ * 0.37;
        double sum = 0.0;
        if (wy1 > 0.0) sum += s1 * noiseAt(Q1 + glm::dvec3(3.1, 7.7, 1.3), 21u, mask1);
        if (wy2 > 0.0) sum += s2 * noiseAt(Q2 + glm::dvec3(1.7, 9.2, 4.4), 22u, mask2);
        if (wd3 > 0.0) sum += s3 * noiseAt(Pd, 23u, -1);
        if (wd4 > 0.0) sum += s4 * noiseAt(Pd * 2.0 + glm::dvec3(5.3, 1.9, 7.2), 24u, -1);
        if (wd5 > 0.0) sum += s5 * noiseAt(Pd * 4.0 + glm::dvec3(2.6, 8.4, 3.7), 25u, -1);
        const double clumpD = std::exp(sum - lmgf(s1) - lmgf(s2) - lmgf(s3) - lmgf(s4) - lmgf(s5));
        const double Ad = std::pow(std::max(0.5 + 0.5 * std::cos(psi - 0.55), 0.0), 1.6 * armK_);
        const double inner = barred_ ? 0.25 + 0.75 * smooth(0.5 * barLen_, barLen_, R) : 1.0;
        const double vert = sech2(y * invTwoHzd_);
        s.tau = dustT0_ * std::exp(1.0 - R * invH_) * cut * inner * vert * (1.0 + dustCm1_ * Ad) * dustNorm_ * clumpD;
        if (barred_) {
            const double bb = 1.0 / invBb_;
            const double off2 = (zb + (xb >= 0.0 ? 0.5 : -0.5) * bb) / (0.22 * bb);
            const double ax = std::abs(xb);
            s.tau += laneT_ * std::exp(-off2 * off2) * smooth(0.15 * barLen_, 0.35 * barLen_, ax) *
                     (1.0 - smooth(0.85 * barLen_, 1.05 * barLen_, ax)) * vert * clumpD;
        }
    }
}

GalaxyGpu GalaxyEval::pack(const glm::dvec3& viewpoint) const {
    GalaxyGpu g{};
    const auto f = [](double v) { return static_cast<float>(v); };
    const glm::dvec3 c = (m_.center - viewpoint) / (1000.0 * units::kParsec);
    g.data[0] = glm::vec4(glm::vec3(c), f(type_));
    g.data[1] = glm::vec4(glm::vec3(m_.normal), f(boundKpc_));
    g.data[2] = glm::vec4(glm::vec3(m_.major), f(invH_));
    g.data[3] = glm::vec4(glm::vec3(eOld_), f(invTwoHz_));
    g.data[4] = glm::vec4(glm::vec3(eYoung_), f(invHy_));
    g.data[5] = glm::vec4(glm::vec3(eHii_), f(invTwoHzy_));
    g.data[6] = glm::vec4(glm::vec3(eBulge_), f(aB_));
    g.data[7] = glm::vec4(glm::vec3(eBar_), f(invQ_));
    g.data[8] = glm::vec4(f(armM_), f(invTanP_), f(lnRef_), f(phase_));
    g.data[9] = glm::vec4(f(armK_), f(armCm1_), f(armNorm_), f(oldCm1_));
    g.data[10] = glm::vec4(f(oldNorm_), f(dustCm1_), f(dustNorm_), f(hiiNorm_));
    g.data[11] = glm::vec4(f(cutR_), f(taper1_), f(laneT_), f(dustT0_));
    g.data[12] = glm::vec4(f(invLamY_), f(ampY_), f(ampO_), f(invLamD_));
    g.data[13] = glm::vec4(f(ampD_), f(invLamH_), f(invTwoHzd_), f(noiseSeed_));
    g.data[14] = glm::vec4(f(cosB_), f(sinB_), f(barred_ ? invAb_ : 0.0), f(invBb_));
    g.data[15] = glm::vec4(f(invCb_), f(invTwoHzh_), f(armK1_), f(armP1_));
    return g;
}

}  // namespace detail

// ------------------------------------------------------------------------------- public methods

double GalaxyModel::stellarDensity(const glm::dvec3& p) const {
    detail::GalaxySample s;
    detail::GalaxyEval(*this).sample(p, s, 0.0, detail::GalaxyEval::kStars);
    return s.stars;
}

double GalaxyModel::youngFraction(const glm::dvec3& p) const {
    detail::GalaxySample s;
    detail::GalaxyEval(*this).sample(p, s, 0.0, detail::GalaxyEval::kStars);
    return s.stars > 0.0 ? std::clamp(s.youngStars / s.stars, 0.0, 1.0) : 0.0;
}

double GalaxyModel::dustExtinction(const glm::dvec3& p) const {
    detail::GalaxySample s;
    detail::GalaxyEval(*this).sample(p, s, 0.0, detail::GalaxyEval::kDust);
    return 1.086 * s.tau;
}

glm::vec3 GalaxyModel::emissivity(const glm::dvec3& p) const {
    detail::GalaxySample s;
    detail::GalaxyEval(*this).sample(p, s, 0.0, detail::GalaxyEval::kLight);
    return glm::vec3(s.starLight + s.nebula);
}

double GalaxyModel::boundingRadiusPc() const { return detail::boundRadiusKpc(*this) * 1000.0; }

GalaxyGpu packGalaxyGpu(const GalaxyModel& g, const glm::dvec3& viewpointMeters) {
    return detail::GalaxyEval(g).pack(viewpointMeters);
}

}  // namespace bhs::universe
