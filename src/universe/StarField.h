#pragma once
// Procedural stars drawn from the galaxy models of a UniverseContent, generated lazily in
// hierarchical cells around the viewpoint (bright, rare stars in large cells that reach far;
// faint, common stars in small cells close by). Used by both the invented and the real universe;
// in the real one, stars already covered by the catalogue are suppressed (catalogCenter/Radius).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "universe/Universe.h"

namespace bhs::universe {

class StarField {
public:
    StarField();
    ~StarField();
    StarField(const StarField&) = delete;
    StarField& operator=(const StarField&) = delete;

    // Selects the density models (host + other galaxies) and resets the cache.
    void configure(const UniverseContent& content, uint32_t seed);

    // Writes into `out` every procedural star that may be brighter than `limitingAppMag` (apparent
    // V magnitude) as seen from `viewpoint` (meters, galactic frame), at most `maxStars` of them,
    // brightest first when the cap applies. Deterministic: a given cell always yields the same
    // stars. Returns true if the set differs from the previous call. Amortised cost <= ~3 ms when
    // the viewpoint moves slowly; must stay usable while travelling across a galaxy every frame.
    bool gather(const glm::dvec3& viewpoint, float limitingAppMag, size_t maxStars, std::vector<StarRecord>& out);

    size_t cachedCells() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bhs::universe
