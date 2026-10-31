// Placeholder StarField (used only while src/universe/StarField.cpp is missing): no procedural
// stars; the universe shows only its explicit stars.
#include "universe/StarField.h"

namespace bhs::universe {

struct StarField::Impl {};

StarField::StarField() : impl_(std::make_unique<Impl>()) {}
StarField::~StarField() = default;
void StarField::configure(const UniverseContent&, uint32_t) {}
bool StarField::gather(const glm::dvec3&, float, size_t, std::vector<StarRecord>& out) {
    const bool changed = !out.empty();
    out.clear();
    return changed;
}
size_t StarField::cachedCells() const { return 0; }

}  // namespace bhs::universe
