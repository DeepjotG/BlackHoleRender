#pragma once
// Spheres drawn by the ray tracer: planets (lit by a light source, procedural surfaces) and stars
// (emissive). The app fills a list of BodyDesc every frame; packBodies converts it to the GPU
// layout of shaders/bodies.glsl, computing camera-relative positions in double precision.
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "universe/Universe.h"

namespace bhs {

enum class BodyKind : int32_t { Planet = 0, Star = 1 };

struct BodyDesc {
    std::string name;
    BodyKind kind = BodyKind::Planet;
    glm::dvec3 pos{0.0};                 // renderer frame, M (relative to the black hole)
    glm::dvec3 vel{0.0};                 // M per M of coordinate time (for riding along)
    double radius = 1e-4;                // M
    universe::PlanetLook look = universe::PlanetLook::Rocky;
    uint32_t seed = 1;
    glm::vec3 tint{1.0f};
    float atmosphere = 0.0f;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};    // spin / ring axis
    float rotation = 0.0f;               // radians about the axis
    glm::vec3 lightDir{0.0f, 1.0f, 0.0f};// unit, from the body towards its light
    glm::vec3 lightColor{1.0f};
    float lightIntensity = 1.0f;         // planets: irradiance scale; stars: surface brightness scale
    float teff = 5772.0f;                // stars
    double ringInner = 0.0, ringOuter = 0.0;  // M
    bool collidable = true;
    std::string info;                    // one-line description for the travel panel
};

struct BodyGpu {
    glm::vec4 posRadius;
    glm::vec4 relPos;
    glm::vec4 look;
    glm::vec4 tint;
    glm::vec4 light;
    glm::vec4 lightColor;
    glm::vec4 axis;
    glm::vec4 ring;
};
static_assert(sizeof(BodyGpu) == 128, "BodyGpu must match struct Body in shaders/bodies.glsl");

// Converts to the GPU layout. Bodies whose apparent radius from the camera is below
// `minAngularRadius` (radians) are skipped (they are drawn as points elsewhere, or invisible).
std::vector<BodyGpu> packBodies(const std::vector<BodyDesc>& bodies, const glm::dvec3& camPos,
                                double minAngularRadius, size_t maxBodies);

}  // namespace bhs
