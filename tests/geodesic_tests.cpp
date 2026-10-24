// Physics checks for the Schwarzschild geodesic integrator (CPU mirror of the shader).
#include <cmath>
#include <cstdio>
#include <string>

#include "physics/Geodesic.h"

using namespace bhs::physics;
using glm::dvec3;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (cond) {                                                                  \
            std::printf("  ok    %s\n", msg);                                        \
        } else {                                                                     \
            ++g_failures;                                                            \
            std::printf("  FAIL  %s   (%s:%d)\n", msg, __FILE__, __LINE__);          \
        }                                                                            \
    } while (0)

// Camera on the +z axis at distance D, ray in the xz-plane with impact parameter b.
static dvec3 dirForImpactParameter(double D, double b) {
    const double sinA = b * std::sqrt(1.0 - 2.0 / D) / D;
    const double a = std::asin(std::min(1.0, sinA));
    return dvec3(std::sin(a), 0.0, -std::cos(a));
}

static double angleBetween(const dvec3& a, const dvec3& b) {
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0));
}

static void testImpactParameterRoundTrip() {
    std::printf("impact parameter mapping\n");
    for (double D : {10.0, 25.0, 2000.0}) {
        for (double b : {1.0, 5.196, 8.0}) {
            if (b * std::sqrt(1.0 - 2.0 / D) / D >= 1.0) continue;
            const dvec3 dir = dirForImpactParameter(D, b);
            const double bb = impactParameter(dvec3(0, 0, D), dir);
            char msg[128];
            std::snprintf(msg, sizeof msg, "b=%.3f from r=%.0f reproduces b (got %.5f)", b, D, bb);
            CHECK(std::abs(bb - b) < 1e-9 * std::max(1.0, b), msg);
        }
    }
}

static void testWeakFieldDeflection() {
    std::printf("weak-field deflection (4/b + 15 pi / (4 b^2))\n");
    const double D = 2000.0;
    for (double b : {50.0, 100.0, 200.0}) {
        const dvec3 cam(0, 0, D);
        const dvec3 dir = dirForImpactParameter(D, b);
        const TraceResult res = trace(cam, dir, 400000, 0.002, 1e-6, 50.0);
        const double expected = 4.0 / b + 15.0 * M_PI / (4.0 * b * b);
        const double got = angleBetween(dir, res.dir);
        const double rel = std::abs(got - expected) / expected;
        char msg[160];
        std::snprintf(msg, sizeof msg, "b=%.0f: escaped, deflection %.6f vs %.6f (rel err %.4f)", b, got, expected, rel);
        CHECK(res.status == TraceResult::Escaped && rel < 0.01, msg);
    }
}

static void testCaptureThreshold() {
    std::printf("capture threshold b_crit = 3 sqrt(3) = 5.196\n");
    for (double D : {10.0, 25.0, 2000.0}) {
        const dvec3 cam(0, 0, D);
        const TraceResult in = trace(cam, dirForImpactParameter(D, 5.15), 400000, 0.002, 1e-4, 30.0);
        const TraceResult out = trace(cam, dirForImpactParameter(D, 5.25), 400000, 0.002, 1e-4, 30.0);
        char msg[128];
        std::snprintf(msg, sizeof msg, "from r=%.0f: b=5.15 captured", D);
        CHECK(in.status == TraceResult::Captured, msg);
        std::snprintf(msg, sizeof msg, "from r=%.0f: b=5.25 escapes (min r %.3f)", D, out.minR);
        CHECK(out.status == TraceResult::Escaped, msg);
    }
}

static void testShadowAngularSize() {
    std::printf("shadow angular radius matches ray capture (static observer)\n");
    for (double r : {4.0, 6.0, 25.0, 100.0}) {
        const double a = shadowAngularRadius(r);
        const dvec3 cam(0, 0, r);
        const double da = 0.004;  // rad
        const dvec3 dirIn(std::sin(a - da), 0.0, -std::cos(a - da));
        const dvec3 dirOut(std::sin(a + da), 0.0, -std::cos(a + da));
        const TraceResult in = trace(cam, dirIn, 400000, 0.002, 1e-4, 30.0);
        const TraceResult out = trace(cam, dirOut, 400000, 0.002, 1e-4, 30.0);
        char msg[160];
        std::snprintf(msg, sizeof msg, "r=%.0f: edge %.3f deg; inside captured / outside escapes", r, a * 180.0 / M_PI);
        CHECK(in.status == TraceResult::Captured && out.status == TraceResult::Escaped, msg);
    }
}

static void testPhotonSphere() {
    std::printf("photon sphere: tangential ray at r=3 stays circular for two orbits\n");
    dvec3 x(3.0, 0.0, 0.0);
    dvec3 v = initialVelocity(x, dvec3(0.0, 0.0, 1.0));  // purely tangential
    const dvec3 hv = glm::cross(x, v);
    const double h2 = glm::dot(hv, hv);
    double maxDev = 0.0;
    const double ds = 0.01;
    const double pathLen = 2.0 * 2.0 * M_PI * 3.0;
    for (double s = 0.0; s < pathLen; s += ds) {
        rk4Step(x, v, h2, ds / glm::length(v));
        maxDev = std::max(maxDev, std::abs(glm::length(x) - 3.0));
    }
    char msg[128];
    std::snprintf(msg, sizeof msg, "max |r-3| over two orbits = %.2e", maxDev);
    CHECK(maxDev < 1e-4, msg);
}

static void testRedshift() {
    std::printf("Doppler/gravitational redshift of disk gas\n");
    const dvec3 cam(0, 0, 30);
    const dvec3 axis(0, 1, 0);  // disk angular velocity along +y
    // Gas at (+8,0,0) moves toward -z (away from the camera); gas at (-8,0,0) moves toward +z (approaching).
    const dvec3 dirAway = glm::normalize(dvec3(8, 0, 0) - cam);
    const dvec3 dirToward = glm::normalize(dvec3(-8, 0, 0) - cam);
    const double bAway = impactParameterAboutAxis(cam, dirAway, axis);
    const double bToward = impactParameterAboutAxis(cam, dirToward, axis);
    char msg[160];
    std::snprintf(msg, sizeof msg, "receding side has L_y/E < 0 (%.3f), approaching side > 0 (%.3f)", bAway, bToward);
    CHECK(bAway < 0.0 && bToward > 0.0, msg);
    std::snprintf(msg, sizeof msg, "|L_y/E| ~ 8.0 for a ray aimed at r=8 (got %.3f)", std::abs(bAway));
    CHECK(std::abs(std::abs(bAway) - 8.0) < 0.15, msg);
    const double gAway = redshiftFactor(8.0, bAway, 30.0);
    const double gToward = redshiftFactor(8.0, bToward, 30.0);
    std::snprintf(msg, sizeof msg, "g(receding)=%.3f < 1 < g(approaching)=%.3f", gAway, gToward);
    CHECK(gAway < 1.0 && gToward > 1.0, msg);
    // Closed form: g = 1 / (sqrt(1-2/rc) * (1-3/r)^-1/2 * (1 - r^-1.5 * b))
    const double expectAway = 1.0 / (std::sqrt(1 - 2.0 / 30) / std::sqrt(1 - 3.0 / 8) * (1 - std::pow(8.0, -1.5) * bAway));
    std::snprintf(msg, sizeof msg, "g(receding) matches closed form (%.4f vs %.4f)", gAway, expectAway);
    CHECK(std::abs(gAway - expectAway) < 1e-9, msg);
    // Face-on view (camera on the axis) sees no Doppler shift, only gravitational redshift.
    const dvec3 camTop(0, 30, 0);
    const double bTop = impactParameterAboutAxis(camTop, glm::normalize(dvec3(8, 0, 0) - camTop), axis);
    const double gTop = redshiftFactor(8.0, bTop, 30.0);
    const double expectTop = std::sqrt(1 - 3.0 / 8) / std::sqrt(1 - 2.0 / 30);
    std::snprintf(msg, sizeof msg, "face-on: L_y/E ~ 0 (%.2e), g = sqrt(1-3/r)/sqrt(1-2/rc) = %.4f (got %.4f)", bTop, expectTop, gTop);
    CHECK(std::abs(bTop) < 1e-9 && std::abs(gTop - expectTop) < 1e-9, msg);
}

static void testFarFieldCorrection() {
    std::printf("far-field correction makes the escape radius irrelevant\n");
    const double D = 2000.0;
    const dvec3 cam(0, 0, D);
    for (double b : {8.0, 20.0, 60.0}) {
        const dvec3 dir = dirForImpactParameter(D, b);
        const TraceResult early = trace(cam, dir, 400000, 0.002, 1e-2, 12.0);   // leaves very early
        const TraceResult late = trace(cam, dir, 400000, 0.002, 1e-7, 12.0);    // leaves very late
        const double diff = angleBetween(early.dir, late.dir);
        char msg[160];
        std::snprintf(msg, sizeof msg, "b=%.0f: early vs late exit directions differ by %.2e rad (steps %d vs %d)", b, diff,
                      early.steps, late.steps);
        CHECK(early.status == TraceResult::Escaped && late.status == TraceResult::Escaped && diff < 5e-4, msg);
    }
}

static void testDiskImagesAboveAndBelow() {
    std::printf("rays just outside the shadow cross the equatorial plane behind the hole\n");
    const dvec3 cam(0.0, 2.6, 24.9);
    const double rc = glm::length(cam);
    const dvec3 toHole = -cam / rc;
    const dvec3 up = glm::normalize(dvec3(0, 1, 0) - toHole * glm::dot(dvec3(0, 1, 0), toHole));
    const double a = shadowAngularRadius(rc) + 0.01;
    const dvec3 dirAbove = glm::normalize(toHole * std::cos(a) + up * std::sin(a));
    const dvec3 dirBelow = glm::normalize(toHole * std::cos(a) - up * std::sin(a));
    const TraceResult above = trace(cam, dirAbove, 400000, 0.002, 1e-4, 30.0);
    const TraceResult below = trace(cam, dirBelow, 400000, 0.002, 1e-4, 30.0);
    char msg[160];
    std::snprintf(msg, sizeof msg, "ray over the top: escaped with %d equatorial crossings", above.equatorCrossings);
    CHECK(above.status == TraceResult::Escaped && above.equatorCrossings >= 1, msg);
    std::snprintf(msg, sizeof msg, "ray under the bottom: escaped with %d equatorial crossings", below.equatorCrossings);
    CHECK(below.status == TraceResult::Escaped && below.equatorCrossings >= 1, msg);
    // Without lensing neither ray would cross the plane behind the hole (the camera is above the plane
    // and both rays are aimed away from it or pass the hole before reaching the plane).
}

int main() {
    testImpactParameterRoundTrip();
    testWeakFieldDeflection();
    testCaptureThreshold();
    testShadowAngularSize();
    testPhotonSphere();
    testRedshift();
    testFarFieldCorrection();
    testDiskImagesAboveAndBelow();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
