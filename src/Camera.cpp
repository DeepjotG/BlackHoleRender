#include "Camera.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace bhs {

namespace {
glm::quat lookRotation(const glm::vec3& from, const glm::vec3& target, const glm::vec3& up) {
    glm::vec3 f = glm::normalize(target - from);
    glm::vec3 u = up;
    if (std::abs(glm::dot(f, glm::normalize(u))) > 0.999f) u = glm::vec3(0, 0, -1);
    glm::vec3 r = glm::normalize(glm::cross(f, u));
    glm::vec3 uu = glm::cross(r, f);
    // Columns: right, up, back (camera looks down -Z).
    glm::mat3 m(r, uu, -f);
    return glm::normalize(glm::quat_cast(m));
}
}  // namespace

Camera::Camera() {
    const auto& b = defaultBookmarks()[0];
    pos_ = b.pos;
    orient_ = b.orient;
}

const std::array<Bookmark, 4>& Camera::defaultBookmarks() {
    static const std::array<Bookmark, 4> bms = [] {
        std::array<Bookmark, 4> a;
        const glm::vec3 origin(0.0f);
        glm::vec3 p0(0.0f, 2.6f, 24.9f);                      // the "Interstellar" shot, ~6 deg above the disk
        a[0] = {"Interstellar shot", p0, lookRotation(p0, origin, glm::vec3(0, 1, 0))};
        glm::vec3 p1(0.0f, 1.3f, 4.6f);                       // just outside the photon sphere, looking past the hole
        a[1] = {"Photon sphere skim", p1, lookRotation(p1, glm::vec3(5.5f, 0.4f, 1.0f), glm::vec3(0, 1, 0))};
        glm::vec3 p2(0.0f, 32.0f, 0.001f);                    // top-down
        a[2] = {"Top-down", p2, lookRotation(p2, origin, glm::vec3(0, 0, -1))};
        glm::vec3 p3(120.0f, 60.0f, 480.0f);                  // far away
        a[3] = {"Far view", p3, lookRotation(p3, origin, glm::vec3(0, 1, 0))};
        return a;
    }();
    return bms;
}

float Camera::speedScale() const {
    const float r = glm::length(pos_);
    return baseSpeed * std::clamp((r - 2.0f) * 0.35f, 0.05f, 3000.0f);
}

glm::mat3 Camera::basis() const { return glm::mat3_cast(orient_); }

void Camera::lookAt(const glm::vec3& target, const glm::vec3& up) {
    orient_ = lookRotation(pos_, target, up);
    moved_ = true;
}

void Camera::setPose(const glm::vec3& pos, const glm::quat& orient) {
    pos_ = pos;
    orient_ = glm::normalize(orient);
    vel_ = glm::vec3(0.0f);
    transitioning_ = false;
    moved_ = true;
}

void Camera::flyTo(const Bookmark& b, float seconds) {
    transitioning_ = true;
    transT_ = 0.0f;
    transDur_ = std::max(0.05f, seconds);
    fromPos_ = pos_;
    toPos_ = b.pos;
    fromQ_ = orient_;
    toQ_ = b.orient;
    vel_ = glm::vec3(0.0f);
    autoOrbit_ = false;
}

void Camera::update(float dt, const Input& in) {
    const glm::vec3 prevPos = pos_;
    const glm::quat prevQ = orient_;

    if (transitioning_) {
        transT_ += dt;
        float t = std::clamp(transT_ / transDur_, 0.0f, 1.0f);
        float e = t * t * (3.0f - 2.0f * t);  // smoothstep ease
        // Interpolate radius logarithmically so long flights feel even.
        float r0 = std::max(glm::length(fromPos_), kMinRadius), r1 = std::max(glm::length(toPos_), kMinRadius);
        glm::vec3 d0 = fromPos_ / glm::length(fromPos_), d1 = toPos_ / glm::length(toPos_);
        // Slerp the position direction: a normalised lerp concentrates the angular motion
        // around the midpoint for near-antipodal end points and is undefined at 180 degrees.
        glm::vec3 axis = glm::cross(d0, d1);
        const float s = glm::length(axis);
        const float ang = std::atan2(s, glm::dot(d0, d1));
        glm::vec3 d = d0;
        if (ang > 1e-5f) {
            if (s < 1e-6f) axis = glm::cross(d0, std::abs(d0.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
            d = glm::angleAxis(ang * e, axis / glm::length(axis)) * d0;
        }
        float r = std::exp(glm::mix(std::log(r0), std::log(r1), e));
        pos_ = d * r;
        orient_ = glm::slerp(fromQ_, toQ_, e);
        if (t >= 1.0f) {
            pos_ = toPos_;
            orient_ = toQ_;
            transitioning_ = false;
        }
    } else if (autoOrbit_) {
        // Rotate position about the Y axis and look at the hole.
        float ang = autoOrbitSpeed * dt;
        glm::quat q = glm::angleAxis(ang, glm::vec3(0, 1, 0));
        pos_ = q * pos_;
        lookAt(glm::vec3(0.0f));
        // Allow radial motion with W/S while orbiting.
        float rs = speedScale() * (in.boost ? 5.0f : 1.0f);
        glm::vec3 radial = glm::normalize(pos_);
        pos_ += radial * (in.move.z * rs * dt);
        pos_ += glm::vec3(0, 1, 0) * (in.move.y * rs * dt);
    } else {
        // Look
        const glm::vec3 localUp(0, 1, 0), localRight(1, 0, 0), localBack(0, 0, 1);
        orient_ = orient_ * glm::angleAxis(-in.lookDelta.x, localUp);
        orient_ = orient_ * glm::angleAxis(-in.lookDelta.y, localRight);
        // Roll with inertia
        float rollTarget = in.roll * 1.6f;
        rollVel_ += (rollTarget - rollVel_) * (1.0f - std::exp(-dt * 6.0f));
        orient_ = orient_ * glm::angleAxis(-rollVel_ * dt, localBack);
        orient_ = glm::normalize(orient_);

        // Move with inertia; speed scales with distance to the horizon.
        float rs = speedScale() * (in.boost ? 5.0f : 1.0f);
        glm::vec3 target = basis() * in.move * rs;
        float k = 1.0f - std::exp(-dt * 7.0f);
        vel_ += (target - vel_) * k;
        pos_ += vel_ * dt;
        // Snap inertia to exact rest once it is sub-pixel so the pose becomes bit-identical and
        // progressive supersampling can start about a second sooner.
        if (glm::dot(in.move, in.move) == 0.0f && glm::length(vel_) < 5e-3f * speedScale()) vel_ = glm::vec3(0.0f);
        if (in.roll == 0.0f && std::abs(rollVel_) < 5e-3f) rollVel_ = 0.0f;
    }

    // Soft barrier outside the horizon: the static-observer camera cannot cross it.
    float r = glm::length(pos_);
    if (r < kMinRadius) {
        glm::vec3 n = pos_ / std::max(r, 1e-6f);
        pos_ = n * kMinRadius;
        float vin = glm::dot(vel_, n);
        if (vin < 0.0f) vel_ -= n * vin;
    }
    if (r > 20000.0f) {
        pos_ = glm::normalize(pos_) * 20000.0f;
    }

    moved_ = transitioning_ || autoOrbit_ || glm::dot(in.lookDelta, in.lookDelta) != 0.0f ||
             glm::dot(vel_, vel_) != 0.0f || rollVel_ != 0.0f ||
             (glm::distance(prevPos, pos_) > 1e-7f * std::max(1.0f, r)) ||
             (std::abs(glm::dot(prevQ, orient_)) < 1.0f - 1e-9f);
}

}  // namespace bhs
