#include "BlackHoleSystem.h"

#include <cmath>
#include <limits>

#include "Units.h"

namespace bhs {

namespace {
constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }
}  // namespace

BlackHoleSystem::BlackHoleSystem() { resetPlanets(); }

void BlackHoleSystem::resetPlanets() {
    using universe::PlanetLook;
    planets.clear();
    Planet miller;
    miller.name = "Miller";
    miller.orbitRadiusM = 6.5;          // just outside the innermost stable circular orbit (6 M)
    miller.inclinationDeg = 7.0;        // keeps it mostly above the thin disk
    miller.nodeDeg = 20.0;
    miller.phase = 0.6;
    miller.radiusKm = 8000.0;
    miller.massEarth = 1.3;
    miller.look = PlanetLook::Ocean;
    miller.tint = glm::vec3(0.75f, 0.9f, 1.0f);
    miller.atmosphere = 0.8f;
    miller.seed = 11;
    planets.push_back(miller);

    Planet mann;
    mann.name = "Mann";
    mann.orbitRadiusM = 60.0;
    mann.inclinationDeg = -4.0;
    mann.nodeDeg = 140.0;
    mann.phase = 2.3;
    mann.radiusKm = 6100.0;
    mann.massEarth = 0.85;
    mann.look = PlanetLook::Ice;
    mann.tint = glm::vec3(0.92f, 0.96f, 1.0f);
    mann.atmosphere = 0.35f;
    mann.seed = 23;
    planets.push_back(mann);

    Planet edmunds;
    edmunds.name = "Edmunds";
    edmunds.orbitRadiusM = 250.0;
    edmunds.inclinationDeg = 3.0;
    edmunds.nodeDeg = 260.0;
    edmunds.phase = 4.1;
    edmunds.radiusKm = 7000.0;
    edmunds.massEarth = 1.1;
    edmunds.look = PlanetLook::Desert;
    edmunds.tint = glm::vec3(1.0f, 0.86f, 0.7f);
    edmunds.atmosphere = 0.5f;
    edmunds.seed = 37;
    planets.push_back(edmunds);
    refresh();
}

void BlackHoleSystem::update(double dtM) {
    for (Planet& p : planets) {
        const double r = std::max(p.orbitRadiusM, 3.0001);
        const double omega = std::pow(r, -1.5) * (p.retrograde ? -1.0 : 1.0);
        p.phase = std::fmod(p.phase + omega * dtM, 2.0 * kPi);
    }
    refresh();
}

void BlackHoleSystem::refresh() {
    for (Planet& p : planets) {
        const double r = std::max(p.orbitRadiusM, 3.0001);
        const double inc = rad(p.inclinationDeg), node = rad(p.nodeDeg);
        // Orbit plane basis: start in the XZ plane, tilt about the node line, rotate the node about +Y.
        const glm::dvec3 nodeDir(std::cos(node), 0.0, -std::sin(node));          // ascending node
        const glm::dvec3 inPlane0(std::sin(node), 0.0, std::cos(node));           // 90 deg ahead in the disk plane
        const glm::dvec3 up(0.0, 1.0, 0.0);
        const glm::dvec3 inPlane = inPlane0 * std::cos(inc) + up * std::sin(inc);
        p.orbitNormal = glm::normalize(glm::cross(inPlane, nodeDir));
        if (p.orbitNormal.y < 0.0) p.orbitNormal = -p.orbitNormal;
        // Prograde motion has angular momentum along orbitNormal (+Y-ish), i.e. it moves from nodeDir towards
        // -inPlane? Choose the tangent so that cross(pos, vel) points along orbitNormal.
        const double c = std::cos(p.phase), s = std::sin(p.phase);
        const glm::dvec3 e1 = nodeDir;
        const glm::dvec3 e2 = glm::normalize(glm::cross(p.orbitNormal, e1));   // cross(e1, e2) = orbitNormal
        p.pos = (e1 * c + e2 * s) * r;
        const double omega = std::pow(r, -1.5) * (p.retrograde ? -1.0 : 1.0);
        p.vel = (-e1 * s + e2 * c) * (r * omega);
    }
}

double BlackHoleSystem::timeDilationCircularOrbit(double r) {
    if (r <= 3.0) return std::numeric_limits<double>::infinity();
    return 1.0 / std::sqrt(1.0 - 3.0 / r);
}

double BlackHoleSystem::timeDilationStatic(double r) {
    if (r <= 2.0) return std::numeric_limits<double>::infinity();
    return 1.0 / std::sqrt(1.0 - 2.0 / r);
}

double BlackHoleSystem::orbitalPeriodM(double r) { return 2.0 * kPi * std::pow(r, 1.5); }

double BlackHoleSystem::orbitalSpeedLocal(double r) {
    // v = sqrt(M / (r - 2M)) for a circular orbit, measured by a static observer.
    if (r <= 3.0) return 1.0;
    return std::sqrt(1.0 / (r - 2.0));
}

double BlackHoleSystem::planetRadiusM(const Planet& p, double bhMassSolar) {
    return p.radiusKm * 1e3 / units::metersPerM(bhMassSolar);
}

double BlackHoleSystem::rocheLimitM(const Planet& p, double bhMassSolar) {
    const double massRatio = bhMassSolar * units::kSolarMass / (p.massEarth * units::kEarthMass);
    return planetRadiusM(p, bhMassSolar) * std::cbrt(2.0 * massRatio);
}

bool BlackHoleSystem::planetFitsOrbit(const Planet& p, double bhMassSolar) {
    return planetRadiusM(p, bhMassSolar) < 0.25 * p.orbitRadiusM;
}

}  // namespace bhs
