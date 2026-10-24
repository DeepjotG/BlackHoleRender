// Placeholder real universe (used only while src/universe/RealUniverse.cpp is missing).
#include "universe/Universe.h"

namespace bhs::universe {

bool buildRealUniverse(UniverseContent& out, const std::string&, BlackHolePlacement, std::string* error) {
    out = UniverseContent{};
    if (error) *error = "the real-universe catalogues are not built yet";
    return false;
}

}  // namespace bhs::universe
