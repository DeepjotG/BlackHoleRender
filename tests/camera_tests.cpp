// Tests of the camera's flights (src/Camera.cpp): the travel-speed setting scales how long a flight to a
// target or a bookmark takes, and nothing else about it.
#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>

#include "Camera.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

// Flies from `from` to a viewpoint `dist` from `target` at the given time scale; returns the seconds it took
// and where the camera ended.
double flight(float timeScale, const glm::dvec3& from, const glm::dvec3& target, double dist, glm::dvec3& end) {
    bhs::Camera cam;
    cam.setPose(from, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    cam.flightTimeScale = timeScale;
    cam.flyToTarget(target, dist, glm::dvec3(0.0, 0.0, 1.0));
    const float dt = 1.0f / 240.0f;
    double t = 0.0;
    bhs::Camera::Input in;
    while (cam.isTransitioning() && t < 120.0) {
        cam.update(dt, in, 1.0);
        t += dt;
    }
    end = cam.position();
    return t;
}

double bookmarkFlight(float timeScale) {
    bhs::Camera cam;
    cam.setPose(glm::dvec3(0.0, 2.0, 30.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    cam.flightTimeScale = timeScale;
    cam.flyTo(bhs::Camera::defaultBookmarks()[3]);
    const float dt = 1.0f / 240.0f;
    double t = 0.0;
    bhs::Camera::Input in;
    while (cam.isTransitioning() && t < 120.0) {
        cam.update(dt, in, 1.0);
        t += dt;
    }
    return t;
}

}  // namespace

int main() {
    const glm::dvec3 from(0.0, 3.0, 40.0), target(500.0, 20.0, -300.0);
    glm::dvec3 e1, eSlow, eFast;
    const double t1 = flight(1.0f, from, target, 25.0, e1);
    const double tSlow = flight(4.0f, from, target, 25.0, eSlow);   // travel speed 0.25x
    const double tFast = flight(0.25f, from, target, 25.0, eFast);  // travel speed 4x
    std::printf("  flight to a target: %.2f s at 1x, %.2f s at 0.25x, %.2f s at 4x\n", t1, tSlow, tFast);
    CHECK(t1 >= 1.4 && t1 <= 8.01);
    CHECK(std::abs(tSlow - 4.0 * t1) < 0.02);
    CHECK(std::abs(tFast - 0.25 * t1) < 0.02);
    // Same destination whatever the pace.
    const glm::dvec3 want = target + glm::dvec3(0.0, 0.0, 1.0) * 25.0;
    CHECK(glm::length(e1 - want) < 1e-6 && glm::length(eSlow - want) < 1e-6 && glm::length(eFast - want) < 1e-6);
    // Bookmarks too.
    const double b1 = bookmarkFlight(1.0f), b2 = bookmarkFlight(2.0f);
    std::printf("  bookmark flight: %.2f s at 1x, %.2f s at 0.5x\n", b1, b2);
    CHECK(b1 > 0.5 && std::abs(b2 - 2.0 * b1) < 0.02);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("camera_tests: all passed\n");
    return 0;
}
