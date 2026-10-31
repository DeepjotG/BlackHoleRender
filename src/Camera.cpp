#include "Camera.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace bhs {

namespace {
glm::quat lookRotation(const glm::dvec3& from, const glm::dvec3& target, const glm::vec3& up) {
    const glm::dvec3 fd = target - from;
    const double len = glm::length(fd);
    glm::vec3 f = len > 0.0 ? glm::vec3(fd / len) : glm::vec3(0, 0, -1);
    glm::vec3 u = up;
    if (std::abs(glm::dot(f, glm::normalize(u))) > 0.999f) u = glm::vec3(0, 0, -1);
    glm::vec3 r = glm::normalize(glm::cross(f, u));
    glm::vec3 uu = glm::cross(r, f);
    // Columns: right, up, back (camera looks down -Z).
    glm::mat3 m(r, uu, -f);
    return glm::normalize(glm::quat_cast(m));
}

glm::dvec3 safeNormalize(const glm::dvec3& v, const glm::dvec3& fallback) {
    const double l = glm::length(v);
    return l > 0.0 ? v / l : fallback;
}

// Great-circle interpolation between unit vectors (handles antipodal end points).
glm::dvec3 slerpDir(const glm::dvec3& d0, const glm::dvec3& d1, double e) {
    glm::dvec3 axis = glm::cross(d0, d1);
    const double s = glm::length(axis);
    const double ang = std::atan2(s, glm::dot(d0, d1));
    if (ang < 1e-9) return d0;
    if (s < 1e-12) axis = glm::cross(d0, std::abs(d0.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
    axis = glm::normalize(axis);
    const double a = ang * e;
    // Rodrigues rotation of d0 about axis (axis is perpendicular to d0).
    return glm::normalize(d0 * std::cos(a) + glm::cross(axis, d0) * std::sin(a));
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
        const glm::dvec3 origin(0.0);
        glm::dvec3 p0(0.0, 2.6, 24.9);                       // the "Interstellar" shot, ~6 deg above the disk
        a[0] = {"Interstellar shot", p0, lookRotation(p0, origin, glm::vec3(0, 1, 0))};
        glm::dvec3 p1(0.0, 1.3, 4.6);                        // just outside the photon sphere, looking past the hole
        a[1] = {"Photon sphere skim", p1, lookRotation(p1, glm::dvec3(5.5, 0.4, 1.0), glm::vec3(0, 1, 0))};
        glm::dvec3 p2(0.0, 32.0, 0.001);                     // top-down
        a[2] = {"Top-down", p2, lookRotation(p2, origin, glm::vec3(0, 0, -1))};
        glm::dvec3 p3(120.0, 60.0, 480.0);                   // far away
        a[3] = {"Far view", p3, lookRotation(p3, origin, glm::vec3(0, 1, 0))};
        return a;
    }();
    return bms;
}

glm::mat3 Camera::basis() const { return glm::mat3_cast(orient_); }

void Camera::lookAt(const glm::dvec3& target, const glm::vec3& up) {
    orient_ = lookRotation(pos_, target, up);
    moved_ = true;
}

void Camera::setPose(const glm::dvec3& pos, const glm::quat& orient) {
    pos_ = pos;
    orient_ = glm::normalize(orient);
    vel_ = glm::dvec3(0.0);
    transitioning_ = false;
    moved_ = true;
}

void Camera::shift(const glm::dvec3& delta) {
    pos_ += delta;
    if (transitioning_) pivot_ += delta;
    moved_ = true;
}

void Camera::pushOutOf(const glm::dvec3& center, double minDist) {
    const glm::dvec3 d = pos_ - center;
    const double l = glm::length(d);
    if (l >= minDist) return;
    const glm::dvec3 n = l > 0.0 ? d / l : glm::dvec3(0.0, 1.0, 0.0);
    pos_ = center + n * minDist;
    const double vin = glm::dot(vel_, n);
    if (vin < 0.0) vel_ -= n * vin;
    moved_ = true;
}

void Camera::flyTo(const Bookmark& b, float seconds) {
    transitioning_ = true;
    transT_ = 0.0f;
    pivot_ = glm::dvec3(0.0);
    fromDist_ = std::max(glm::length(pos_), kMinRadius);
    toDist_ = std::max(glm::length(b.pos), kMinRadius);
    fromDir_ = safeNormalize(pos_, glm::dvec3(0, 0, 1));
    toDir_ = safeNormalize(b.pos, glm::dvec3(0, 0, 1));
    const double ratio = std::abs(std::log(toDist_ / fromDist_));
    transDur_ = std::max(0.05f, std::max(seconds, static_cast<float>(1.2 + 0.25 * ratio)));
    fromQ_ = orient_;
    toQ_ = b.orient;
    lookAtPivot_ = false;
    vel_ = glm::dvec3(0.0);
    autoOrbit_ = false;
}

void Camera::flyToTarget(const glm::dvec3& target, double viewDistance, const glm::dvec3& fromDir) {
    transitioning_ = true;
    transT_ = 0.0f;
    pivot_ = target;
    const glm::dvec3 rel = pos_ - target;
    fromDist_ = std::max(glm::length(rel), 1e-12);
    fromDir_ = safeNormalize(rel, glm::dvec3(0, 0, 1));
    toDir_ = glm::length(fromDir) > 0.0 ? glm::normalize(fromDir) : fromDir_;
    toDist_ = std::max(viewDistance, 1e-12);
    const double ratio = std::abs(std::log(toDist_ / fromDist_));
    const double turn = std::acos(std::clamp(glm::dot(fromDir_, toDir_), -1.0, 1.0));
    transDur_ = static_cast<float>(std::clamp(1.4 + 0.22 * ratio + 0.6 * turn, 1.4, 8.0));
    fromQ_ = orient_;
    toQ_ = lookRotation(target + toDir_ * toDist_, target, glm::vec3(0, 1, 0));
    lookAtPivot_ = true;
    vel_ = glm::dvec3(0.0);
    autoOrbit_ = false;
}

void Camera::retargetFlight(const glm::dvec3& delta) {
    if (transitioning_) pivot_ += delta;
}

void Camera::update(float dt, const Input& in, double speed) {
    const glm::dvec3 prevPos = pos_;
    const glm::quat prevQ = orient_;
    lastSpeed_ = speed;

    if (transitioning_) {
        transT_ += dt;
        const double t = std::clamp(static_cast<double>(transT_ / transDur_), 0.0, 1.0);
        const double e = t * t * (3.0 - 2.0 * t);  // smoothstep ease
        const glm::dvec3 d = slerpDir(fromDir_, toDir_, e);
        const double dist = std::exp(glm::mix(std::log(fromDist_), std::log(toDist_), e));
        pos_ = pivot_ + d * dist;
        if (lookAtPivot_) {
            // Turn towards the target during the first third of the flight, then keep facing it.
            const glm::quat facing = lookRotation(pos_, pivot_, glm::vec3(0, 1, 0));
            orient_ = glm::slerp(fromQ_, facing, static_cast<float>(std::min(1.0, e * 3.0)));
        } else {
            orient_ = glm::slerp(fromQ_, toQ_, static_cast<float>(e));
        }
        if (t >= 1.0) {
            pos_ = pivot_ + toDir_ * toDist_;
            orient_ = lookAtPivot_ ? lookRotation(pos_, pivot_, glm::vec3(0, 1, 0)) : toQ_;
            transitioning_ = false;
        }
    } else if (autoOrbit_) {
        // Rotate position about the Y axis and look at the hole.
        const double ang = autoOrbitSpeed * dt;
        const double c = std::cos(ang), s = std::sin(ang);
        pos_ = glm::dvec3(c * pos_.x + s * pos_.z, pos_.y, -s * pos_.x + c * pos_.z);
        lookAt(glm::dvec3(0.0));
        // Allow radial motion with W/S while orbiting.
        const double rs = speed * (in.boost ? 5.0 : 1.0);
        const glm::dvec3 radial = safeNormalize(pos_, glm::dvec3(0, 0, 1));
        pos_ += radial * (static_cast<double>(in.move.z) * rs * dt);
        pos_ += glm::dvec3(0, 1, 0) * (static_cast<double>(in.move.y) * rs * dt);
    } else {
        // Look
        const glm::vec3 localUp(0, 1, 0), localRight(1, 0, 0), localBack(0, 0, 1);
        orient_ = orient_ * glm::angleAxis(-in.lookDelta.x, localUp);
        orient_ = orient_ * glm::angleAxis(-in.lookDelta.y, localRight);
        // Roll with inertia
        const float rollTarget = in.roll * 1.6f;
        rollVel_ += (rollTarget - rollVel_) * (1.0f - std::exp(-dt * 6.0f));
        orient_ = orient_ * glm::angleAxis(-rollVel_ * dt, localBack);
        orient_ = glm::normalize(orient_);

        // Move with inertia at the speed the app chose for this location.
        const double rs = speed * (in.boost ? 5.0 : 1.0);
        const glm::dvec3 target = glm::dvec3(basis() * in.move) * rs;
        const double k = 1.0 - std::exp(-static_cast<double>(dt) * 7.0);
        vel_ += (target - vel_) * k;
        pos_ += vel_ * static_cast<double>(dt);
        // Snap inertia to exact rest once it is sub-pixel so the pose becomes bit-identical and
        // progressive supersampling can start about a second sooner.
        if (glm::dot(in.move, in.move) == 0.0f && glm::length(vel_) < 5e-3 * speed) vel_ = glm::dvec3(0.0);
        if (in.roll == 0.0f && std::abs(rollVel_) < 5e-3f) rollVel_ = 0.0f;
    }

    // Soft barrier outside the horizon: the static-observer camera cannot cross it.
    const double r = glm::length(pos_);
    if (r < kMinRadius) {
        const glm::dvec3 n = safeNormalize(pos_, glm::dvec3(0, 0, 1));
        pos_ = n * kMinRadius;
        const double vin = glm::dot(vel_, n);
        if (vin < 0.0) vel_ -= n * vin;
    }
    if (r > maxRadius) pos_ = safeNormalize(pos_, glm::dvec3(0, 0, 1)) * maxRadius;

    moved_ = moved_ || transitioning_ || autoOrbit_ || glm::dot(in.lookDelta, in.lookDelta) != 0.0f ||
             glm::dot(vel_, vel_) != 0.0 || rollVel_ != 0.0f ||
             (glm::distance(prevPos, pos_) > 1e-12 * std::max(1.0, r)) ||
             (std::abs(glm::dot(prevQ, orient_)) < 1.0f - 1e-9f);
}

}  // namespace bhs
