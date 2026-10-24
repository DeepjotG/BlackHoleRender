#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <string>

namespace bhs {

struct Bookmark {
    std::string name;
    glm::dvec3 pos;
    glm::quat orient;
};

// Free-flight 6DOF camera with inertia. Position is in the renderer frame (black hole at the
// origin, +Y spin axis, lengths in M) and kept in double precision so the same camera can hover a
// few metres above a planet or fly between galaxies.
class Camera {
public:
    struct Input {
        glm::vec3 move{0.0f};      // local axes: +x right, +y up, +z backward
        float roll = 0.0f;         // -1..1
        bool boost = false;
        glm::vec2 lookDelta{0.0f}; // radians (yaw, pitch)
    };

    Camera();

    // `speed` is the flight speed in M per second at full stick (the app derives it from the
    // distance to the nearest surface).
    void update(float dt, const Input& in, double speed);
    void lookAt(const glm::dvec3& target, const glm::vec3& up = glm::vec3(0, 1, 0));
    void setPose(const glm::dvec3& pos, const glm::quat& orient);
    void flyTo(const Bookmark& b, float seconds = 1.6f);
    // Flies to `viewDistance` from `target`, ending up looking at it from direction `fromDir`
    // (unit, from the target towards the camera; zero = keep the current bearing).
    void flyToTarget(const glm::dvec3& target, double viewDistance, const glm::dvec3& fromDir = glm::dvec3(0.0));
    // Moves the flight's destination along with a moving target (call every frame during a flight).
    void retargetFlight(const glm::dvec3& delta);
    void cancelFlight() { transitioning_ = false; }
    bool isTransitioning() const { return transitioning_; }
    // Translates the camera without counting it as user motion (riding along with a moving body).
    void shift(const glm::dvec3& delta);
    // Keeps the camera at least `minDist` from `center` (planet surfaces), removing inward velocity.
    void pushOutOf(const glm::dvec3& center, double minDist);

    // Auto-orbit: circle the hole at the current radius, always looking at it.
    void setAutoOrbit(bool on) {
        autoOrbit_ = on;
        vel_ = glm::dvec3(0.0);
        rollVel_ = 0.0f;
    }
    bool autoOrbit() const { return autoOrbit_; }
    float autoOrbitSpeed = 0.08f;  // rad/s

    glm::mat3 basis() const;       // columns: right, up, back
    glm::vec3 forward() const { return orient_ * glm::vec3(0, 0, -1); }
    glm::dvec3 position() const { return pos_; }
    glm::quat orientation() const { return orient_; }
    double radius() const { return glm::length(pos_); }
    glm::dvec3 velocity() const { return vel_; }
    double lastSpeed() const { return lastSpeed_; }   // full-stick speed used in the last update, M/s

    float baseSpeed = 1.0f;        // user multiplier (scroll wheel)
    double maxRadius = 20000.0;    // camera cannot go farther from the hole than this (M)
    static constexpr double kMinRadius = 2.3;

    // Whether the pose changed since the previous update (used to reset accumulation).
    bool movedThisFrame() const { return moved_; }
    void markMoved() { moved_ = true; }

    static const std::array<Bookmark, 4>& defaultBookmarks();

private:
    glm::dvec3 pos_{0.0, 2.6, 24.9};
    glm::quat orient_{1, 0, 0, 0};
    glm::dvec3 vel_{0.0};
    float rollVel_ = 0.0f;
    bool moved_ = true;
    double lastSpeed_ = 1.0;

    bool transitioning_ = false;
    float transT_ = 0.0f, transDur_ = 1.0f;
    // Flights are parameterised around a pivot: position = pivot + dir * dist with the distance
    // interpolated logarithmically and the direction slerped, so trips across many orders of
    // magnitude (planet surface to another galaxy) feel even.
    glm::dvec3 pivot_{0.0};
    glm::dvec3 fromDir_{0.0, 0.0, 1.0}, toDir_{0.0, 0.0, 1.0};
    double fromDist_ = 1.0, toDist_ = 1.0;
    glm::quat fromQ_{}, toQ_{};
    bool lookAtPivot_ = false;

    bool autoOrbit_ = false;
};

}  // namespace bhs
