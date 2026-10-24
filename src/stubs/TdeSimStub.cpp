// Placeholder used when src/tde/TdeSim.cpp is not present yet: the app builds and runs, and the
// star-disruption panel reports that the simulation module is unavailable.
#include "tde/TdeSim.h"

namespace bhs::tde {

struct TdeSim::Impl {
    TdeStats stats;
    GasVolume none;
};

TdeSim::TdeSim() : impl_(std::make_unique<Impl>()) {}
TdeSim::~TdeSim() = default;
bool TdeSim::init(int) { return false; }
void TdeSim::shutdown() {}
void TdeSim::spawn(const StarParams&, const OrbitParams&, double, int) {}
void TdeSim::clear() {}
bool TdeSim::active() const { return false; }
void TdeSim::step(double) {}
const TdeStats& TdeSim::stats() const { return impl_->stats; }
int TdeSim::volumeCount() const { return 0; }
const GasVolume& TdeSim::volume(int) const { return impl_->none; }
bool TdeSim::starIntact() const { return false; }
glm::dvec3 TdeSim::starPosition() const { return glm::dvec3(0.0); }
glm::dvec3 TdeSim::starVelocity() const { return glm::dvec3(0.0); }
double TdeSim::starRadiusM() const { return 0.0; }
double TdeSim::starTeff() const { return 5772.0; }

}  // namespace bhs::tde
