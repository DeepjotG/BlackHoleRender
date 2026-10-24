#include "DiskTextures.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Log.h"

namespace bhs {

namespace {

// Wyman, Sloan & Shirley (2013) multi-lobe Gaussian fits of the CIE 1931 2-degree observer.
double lobe(double lambda, double mu, double s1, double s2) {
    const double s = lambda < mu ? s1 : s2;
    const double t = (lambda - mu) / s;
    return std::exp(-0.5 * t * t);
}
void cieXyz(double lambda, double& x, double& y, double& z) {
    x = 1.056 * lobe(lambda, 599.8, 37.9, 31.0) + 0.362 * lobe(lambda, 442.0, 16.0, 26.7) -
        0.065 * lobe(lambda, 501.1, 20.4, 26.2);
    y = 0.821 * lobe(lambda, 568.8, 46.9, 40.5) + 0.286 * lobe(lambda, 530.9, 16.3, 31.1);
    z = 1.217 * lobe(lambda, 437.0, 11.8, 36.0) + 0.681 * lobe(lambda, 459.0, 26.0, 13.8);
}

// Planck spectral radiance (arbitrary scale) for wavelength in nm, temperature in K.
double planck(double lambdaNm, double T) {
    const double h = 6.62607015e-34, c = 2.99792458e8, k = 1.380649e-23;
    const double l = lambdaNm * 1e-9;
    const double e = std::exp((h * c) / (l * k * T)) - 1.0;
    return (2.0 * h * c * c) / (l * l * l * l * l) / e;
}

void integrate(double T, double& X, double& Y, double& Z) {
    X = Y = Z = 0.0;
    for (int l = 380; l <= 780; ++l) {
        double x, y, z;
        cieXyz(l, x, y, z);
        const double b = planck(l, T);
        X += b * x;
        Y += b * y;
        Z += b * z;
    }
}

}  // namespace

bool DiskTextures::init() {
    bakeBlackbodyLut();
    if (!bakeNoise()) logWarn("3D noise bake failed; the volumetric disk will look flat");
    return lut_ != 0;
}

void DiskTextures::shutdown() {
    noiseProg_.destroy();
    if (lut_) glDeleteTextures(1, &lut_);
    if (noise_) glDeleteTextures(1, &noise_);
    lut_ = noise_ = 0;
}

void DiskTextures::bakeBlackbodyLut() {
    std::vector<float> data(static_cast<size_t>(kLutSize) * 4);
    double Xr, Yr, Zr;
    integrate(6500.0, Xr, Yr, Zr);
    const double lnMin = std::log(kTempMin), lnMax = std::log(kTempMax);
    for (int i = 0; i < kLutSize; ++i) {
        const double t = static_cast<double>(i) / (kLutSize - 1);
        const double T = std::exp(lnMin + (lnMax - lnMin) * t);
        double X, Y, Z;
        integrate(T, X, Y, Z);
        // XYZ -> linear sRGB (D65)
        double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
        double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
        double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
        r = std::max(r, 0.0);
        g = std::max(g, 0.0);
        b = std::max(b, 0.0);
        const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        const double inv = lum > 0.0 ? 1.0 / lum : 0.0;
        data[i * 4 + 0] = static_cast<float>(r * inv);
        data[i * 4 + 1] = static_cast<float>(g * inv);
        data[i * 4 + 2] = static_cast<float>(b * inv);
        data[i * 4 + 3] = static_cast<float>(Y / Yr);  // visible luminance relative to 6500 K
    }
    if (!lut_) glGenTextures(1, &lut_);
    glBindTexture(GL_TEXTURE_1D, lut_);
    glTexStorage1D(GL_TEXTURE_1D, 1, GL_RGBA32F, kLutSize);
    glTexSubImage1D(GL_TEXTURE_1D, 0, 0, kLutSize, GL_RGBA, GL_FLOAT, data.data());
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_1D, 0);
    logInfo("blackbody LUT baked ({} entries, {:.0f}-{:.0f} K)", kLutSize, kTempMin, kTempMax);
}

bool DiskTextures::bakeNoise() {
    if (!noiseProg_.loadCompute("noise_bake.comp")) return false;
    if (!noise_) glGenTextures(1, &noise_);
    glBindTexture(GL_TEXTURE_3D, noise_);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R16F, kNoiseSize, kNoiseSize, kNoiseSize);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    glBindTexture(GL_TEXTURE_3D, 0);

    noiseProg_.use();
    noiseProg_.set("uSize", kNoiseSize);
    noiseProg_.set("uSeed", 7u);
    glBindImageTexture(0, noise_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R16F);
    const GLuint groups = static_cast<GLuint>(kNoiseSize / 8);
    glDispatchCompute(groups, groups, groups);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
    logInfo("3D noise texture baked ({}^3)", kNoiseSize);
    return true;
}

}  // namespace bhs
