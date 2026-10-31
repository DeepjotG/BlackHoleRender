#pragma once
// UI-facing state of the supernova feature (not persisted).
namespace bhs {

struct SnScenario {
    const char* name;
    double starMassSolar, starRadiusSolar, starTeffK;
};
inline const SnScenario kSnScenarios[] = {
    {"Red supergiant (Betelgeuse-like, 700 Suns wide)", 15.0, 700.0, 3500.0},
    {"Blue supergiant (SN 1987A-like, 50 Suns wide)", 18.0, 50.0, 16000.0},
};
inline constexpr int kSnScenarioCount = static_cast<int>(sizeof(kSnScenarios) / sizeof(kSnScenarios[0]));

struct SnControls {
    int scenario = 0;
    bool spawnRequested = false;
    bool clearRequested = false;
    bool watchRequested = false;
    bool autoWarp = true;
    bool moduleAvailable = false;
};

}  // namespace bhs
