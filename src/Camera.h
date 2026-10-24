#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <string>

namespace bhs {

struct Bookmark {
    std::string name;
    glm::vec3 pos;
    glm::quat orient;
};

// Free-flight 6DOF camera with inertia. Position is in Schwarzschild-like
// Cartesian coordinates (G = c = M = 1); the hole sits at the origin and the
// disk lies in the XZ plane (Y is the spin axis).
class Camera {
public:
    struct Input {
        glm::vec3 move{0.0f};      // local axes: +x right, +y up, +z backward
        float roll = 0.0f;         // -1..1
        bool boost = false;
        glm::vec2 lookDelta{0.0f}; // radians (yaw, pitch)
    };

    Camera();

    void update(float dt, const Input& in);
    void lookAt(const glm::vec3& target, const glm::vec3& up = glm::vec3(0, 1, 0));
    void setPose(const glm::vec3& pos, const glm::quat& orient);
    void flyTo(const Bookmark& b, float seconds = 1.6f);
    bool isTransitioning() const { return transitioning_; }

    // Auto-orbit: circle the hole at the current radius, always looking at it.
    void setAutoOrbit(bool on) {
        autoOrbit_ = on;
        vel_ = glm::vec3(0.0f);
        rollVel_ = 0.0f;
    }
    bool autoOrbit() const { return autoOrbit_; }
    float autoOrbitSpeed = 0.08f;  // rad/s

    glm::mat3 basis() const;       // columns: right, up, back
    glm::vec3 forward() const { return orient_ * glm::vec3(0, 0, -1); }
    glm::vec3 position() const { return pos_; }
    glm::quat orientation() const { return orient_; }
    float radius() const { return glm::length(pos_); }
    glm::vec3 velocity() const { return vel_; }
    float speedScale() const;      // current M/s for full stick deflection

    float baseSpeed = 1.0f;        // user multiplier (scroll wheel)
    static constexpr float kMinRadius = 2.3f;

    // Whether the pose changed since the previous update (used to reset accumulation).
    bool movedThisFrame() const { return moved_; }

    static const std::array<Bookmark, 4>& defaultBookmarks();

private:
    glm::vec3 pos_{0.0f, 2.6f, 24.9f};
    glm::quat orient_{1, 0, 0, 0};
    glm::vec3 vel_{0.0f};
    float rollVel_ = 0.0f;
    bool moved_ = true;

    bool transitioning_ = false;
    float transT_ = 0.0f, transDur_ = 1.0f;
    glm::vec3 fromPos_{}, toPos_{};
    glm::quat fromQ_{}, toQ_{};

    bool autoOrbit_ = false;
};

}  // namespace bhs
