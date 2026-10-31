#pragma once
// UI-facing state of the neutron-star merger (gamma-ray burst) feature (not persisted).
namespace bhs {

struct GrbScenario {
    const char* name;
    double bhMassSolar;
    double siteRadiusM;     // where the binary circles the hole
    double jetTiltDeg;      // the jet axis, tilted from the direction of the hole
    double siteInclinationDeg, siteLongitudeDeg;   // the binary's orbit: tilt from the disk plane, start
};
inline const GrbScenario kGrbScenarios[] = {
    {"Beside Sagittarius A* (60 M out, a jet aimed past the hole)", 4.297e6, 60.0, 14.0, 0.0, 200.0},
    {"In Gargantua's grip (10 M out, above the disk, a jet bent round the hole)", 1e8, 10.0, 40.0, 45.0, 270.0},
    {"Far from the hole (400 M out)", 4.297e6, 400.0, 30.0, 0.0, 200.0},
};
inline constexpr int kGrbScenarioCount = static_cast<int>(sizeof(kGrbScenarios) / sizeof(kGrbScenarios[0]));
inline constexpr int kGrbScenarioCustom = kGrbScenarioCount;

struct GrbControls {
    int scenario = 0;
    double siteRadiusM = 60.0;
    double jetTiltDeg = 14.0;
    double siteInclinationDeg = 0.0, siteLongitudeDeg = 200.0;
    double startSeparationKm = 120.0;
    bool spawnRequested = false;
    bool clearRequested = false;
    bool watchRequested = false;
    bool autoWarp = true;          // the director paces the event's own clock
    bool moduleAvailable = false;
    // Selects a ready-made event; returns the black hole mass it needs.
    double applyScenario(int i) {
        scenario = i;
        siteRadiusM = kGrbScenarios[i].siteRadiusM;
        jetTiltDeg = kGrbScenarios[i].jetTiltDeg;
        siteInclinationDeg = kGrbScenarios[i].siteInclinationDeg;
        siteLongitudeDeg = kGrbScenarios[i].siteLongitudeDeg;
        return kGrbScenarios[i].bhMassSolar;
    }
};

}  // namespace bhs
