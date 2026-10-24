// Placeholder GalaxyModel (used only while src/universe/GalaxyModel.cpp is missing): exponential
// disk + bulge, packed for shaders/galaxy_model_fallback.glsl.
#include <cmath>

#include "Units.h"
#include "universe/GalaxyModel.h"

namespace bhs::universe {

namespace {
constexpr double kPi = 3.14159265358979323846;
struct DiskFrame {
    double x, y, z, R;
};
DiskFrame diskFrame(const GalaxyModel& g, const glm::dvec3& p) {
    const glm::dvec3 d = (p - g.center) / units::kParsec;
    const glm::dvec3 w = glm::cross(g.normal, g.major);
    DiskFrame f{glm::dot(d, g.major), glm::dot(d, g.normal), glm::dot(d, w), 0.0};
    f.R = std::sqrt(f.x * f.x + f.z * f.z);
    return f;
}
}  // namespace

double GalaxyModel::stellarDensity(const glm::dvec3& p) const {
    const DiskFrame f = diskFrame(*this, p);
    const double n = stellarMassSolar / 0.4;
    const double disk = n * (1.0 - bulgeMassFraction) / (4.0 * kPi * diskScaleLengthPc * diskScaleLengthPc * diskScaleHeightPc) *
                        std::exp(-f.R / diskScaleLengthPc - std::abs(f.y) / diskScaleHeightPc);
    const double r = std::sqrt(f.R * f.R + f.y * f.y);
    const double bulge = n * bulgeMassFraction / (8.0 * kPi * std::pow(bulgeRadiusPc, 3.0)) * std::exp(-r / bulgeRadiusPc);
    return disk + bulge;
}
double GalaxyModel::youngFraction(const glm::dvec3&) const { return 0.1; }
double GalaxyModel::dustExtinction(const glm::dvec3& p) const {
    const DiskFrame f = diskFrame(*this, p);
    return dustExtinctionPerKpc / 1000.0 * std::exp(1.0 - f.R / diskScaleLengthPc - std::abs(f.y) / dustScaleHeightPc);
}
glm::vec3 GalaxyModel::emissivity(const glm::dvec3& p) const {
    return diskColor * static_cast<float>(stellarDensity(p) * 0.4 * luminositySolar / stellarMassSolar);
}
double GalaxyModel::boundingRadiusPc() const {
    if (type == GalaxyType::Elliptical) return ellipticalRadiusPc * 4.0;
    return std::max(diskCutoffPc, diskScaleLengthPc * 5.0);
}

GalaxyGpu packGalaxyGpu(const GalaxyModel& g, const glm::dvec3& viewpoint) {
    GalaxyGpu out{};
    const glm::dvec3 c = (g.center - viewpoint) / (1000.0 * units::kParsec);
    out.data[0] = glm::vec4(glm::vec3(c), static_cast<float>(static_cast<int>(g.type)));
    out.data[1] = glm::vec4(glm::vec3(g.normal), static_cast<float>(g.diskScaleLengthPc / 1000.0));
    out.data[2] = glm::vec4(glm::vec3(g.major), static_cast<float>(g.diskScaleHeightPc / 1000.0));
    out.data[3] = glm::vec4(static_cast<float>(g.luminositySolar), static_cast<float>(g.bulgeMassFraction),
                            static_cast<float>(g.bulgeRadiusPc / 1000.0), static_cast<float>(g.boundingRadiusPc() / 1000.0));
    out.data[4] = glm::vec4(g.diskColor, static_cast<float>(g.armCount));
    out.data[5] = glm::vec4(g.bulgeColor, static_cast<float>(g.armPitchDeg * kPi / 180.0));
    out.data[6] = glm::vec4(static_cast<float>(g.dustExtinctionPerKpc / 1000.0 / 1.086), static_cast<float>(g.dustScaleHeightPc / 1000.0),
                            static_cast<float>(g.armContrast), static_cast<float>(g.armRefRadiusPc / 1000.0));
    out.data[7] = glm::vec4(g.armColor, static_cast<float>(g.seed % 1000u));
    return out;
}

}  // namespace bhs::universe
