#pragma once
// UI-facing state of the star-disruption feature (not persisted).

namespace bhs {

struct StarPreset {
    const char* name;
    double massSolar, radiusSolar, teff, polytropeN;
};

inline const StarPreset kStarPresets[] = {
    {"Sun-like star", 1.0, 1.0, 5772.0, 3.0},
    {"Red dwarf", 0.3, 0.3, 3300.0, 1.5},
    {"Red giant", 1.2, 100.0, 4100.0, 1.5},
    {"Red supergiant (Betelgeuse-like)", 18.0, 764.0, 3600.0, 1.5},
    {"Blue supergiant", 30.0, 20.0, 25000.0, 3.0},
    {"White dwarf", 0.6, 0.012, 20000.0, 1.5},
};
inline constexpr int kStarPresetCount = static_cast<int>(sizeof(kStarPresets) / sizeof(kStarPresets[0]));

// Ready-made events: black hole mass + star + orbit.
struct TdeScenario {
    const char* name;
    double bhMassSolar;
    int starPreset;
    double beta;
    double inclinationDeg;
};

inline const TdeScenario kTdeScenarios[] = {
    {"Sun-like star vs Sagittarius A* (close, strongly lensed)", 4.297e6, 0, 2.0, 25.0},
    {"Red giant vs Gargantua", 1e8, 2, 3.0, 20.0},
    {"Red supergiant vs Gargantua", 1e8, 3, 2.0, 30.0},
    {"Sun-like star vs Gargantua (swallowed whole)", 1e8, 0, 1.0, 15.0},
    {"White dwarf vs an intermediate-mass hole", 1e4, 5, 1.5, 20.0},
};
inline constexpr int kTdeScenarioCount = static_cast<int>(sizeof(kTdeScenarios) / sizeof(kTdeScenarios[0]));
inline constexpr int kTdeScenarioCustom = kTdeScenarioCount;   // "Custom": the star and orbit as set by hand

struct TdeControls {
    int scenario = 0;              // index into kTdeScenarios, or kTdeScenarioCustom
    int starPreset = 0;
    double massSolar = 1.0, radiusSolar = 1.0, teff = 5772.0, polytropeN = 3.0;
    double beta = 2.0;
    double inclinationDeg = 25.0;
    double longitudeDeg = 200.0;
    bool retrograde = false;
    int particles = 262144;
    bool spawnRequested = false;
    bool clearRequested = false;
    bool watchRequested = false;   // move the camera to a viewpoint that frames the event
    bool autoWarp = true;          // keep the time warp matched to the event's current pace
    bool keepDisk = false;         // keep the steady accretion disk during an event (normally it fades out)
    bool moduleAvailable = false;  // false while the simulation module is not built

    void applyStarPreset(int i) {
        starPreset = i;
        const StarPreset& sp = kStarPresets[i];
        massSolar = sp.massSolar;
        radiusSolar = sp.radiusSolar;
        teff = sp.teff;
        polytropeN = sp.polytropeN;
    }
    // Selects a ready-made event; returns the black hole mass it needs.
    double applyScenario(int i) {
        scenario = i;
        const TdeScenario& sc = kTdeScenarios[i];
        applyStarPreset(sc.starPreset);
        beta = sc.beta;
        inclinationDeg = sc.inclinationDeg;
        return sc.bhMassSolar;
    }
};

}  // namespace bhs
