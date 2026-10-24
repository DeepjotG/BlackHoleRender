#pragma once
// Internals of the procedural star field (src/universe/StarField.cpp) that the tests need to compare
// generated counts with the density model.
namespace bhs::universe::detail {

constexpr int kStarClasses = 24;    // one-magnitude classes of absolute V magnitude ...
constexpr int kStarMinMag = -8;     // ... class k holds M_V in [kStarMinMag + k, kStarMinMag + k + 1)
constexpr float kStarPubMargin = 0.4f;   // published beyond the limiting magnitude when the cap does not apply

// Share of class k among the old (disk, bulge, bar) and the young (arm) populations; each sums to 1.
double starClassFraction(int k, bool young);

}  // namespace bhs::universe::detail
