#pragma once
#include <cstdint>
#include <string>

namespace bhs {

enum DiskMode : int { DISK_NONE = 0, DISK_THIN = 1, DISK_VOLUMETRIC = 2 };
// Disk flow: M of orbital motion per second at rotation speed 1 (the inner edge, period 2 pi 6^1.5 M,
// goes round in about 6 s).
inline constexpr double kDiskFlowRate = 16.0;
enum TonemapMode : int { TONEMAP_AGX = 0, TONEMAP_ACES = 1, TONEMAP_REINHARD = 2, TONEMAP_NONE = 3 };
enum Preset : int { PRESET_LOW = 0, PRESET_MEDIUM = 1, PRESET_HIGH = 2, PRESET_ULTRA = 3, PRESET_CUSTOM = 4 };

// The fixed flight speed's range (m/s): a kilometre a second to a million light-years a second.
inline constexpr double kMinFixedSpeed = 1e3;
inline constexpr double kMaxFixedSpeed = 9.4607304725808e21;

// All tunable parameters. Units: G = c = M = 1 (horizon r = 2, ISCO r = 6).
struct Settings {
    // ---- Quality ----
    int preset = PRESET_HIGH;
    float renderScale = 1.0f;       // internal resolution / window resolution
    int maxSteps = 450;             // geodesic integration step cap per ray
    float stepScale = 0.05f;        // base step length = stepScale * r
    float diskStep = 0.18f;         // step length inside the disk slab
    int diskMode = DISK_VOLUMETRIC;
    int skyResolution = 2048;       // baked cubemap face size
    bool dynamicResolution = false;
    float targetFrameMs = 16.6f;
    bool accumulate = true;         // supersample while the camera is still
    int maxAccumFrames = 256;
    bool vsync = true;

    // ---- Physics ----
    bool lensing = true;
    float dopplerStrength = 0.0f;   // 0 = film look (no Doppler/redshift), 1 = physical

    // ---- Accretion disk ----
    float diskInner = 6.0f;
    float diskOuter = 20.0f;
    float diskThickness = 0.035f;   // scale height H/r
    float diskDensity = 1.0f;
    float diskOpacity = 2.0f;
    float diskPeakTemp = 6000.0f;   // Kelvin at the hottest ring
    float diskBrightness = 5.5f;
    float diskNoiseScale = 1.0f;
    float diskNoiseStrength = 0.85f;
    float diskSpeed = 1.0f;         // disk time multiplier
    float diskWarmth = 0.4f;        // artistic warm tint (0 = pure blackbody colours)

    // ---- Sky ----
    uint32_t skySeed = 1337u;
    int galaxyCount = 4000;
    float galaxyBrightness = 1.0f;
    float milkyWayBrightness = 1.0f;
    float nebulaBrightness = 1.0f;
    float starDensity = 1.0f;
    float starBrightness = 1.0f;
    float skyBrightness = 1.0f;

    // ---- Post ----
    float exposure = 1.15f;
    bool autoExposure = false;
    float bloomIntensity = 0.22f;
    float bloomThreshold = 1.4f;
    int tonemap = TONEMAP_AGX;
    float vignette = 0.35f;
    float grain = 0.02f;
    float chromaticAberration = 0.0f;
    float saturation = 1.15f;
    float sharpen = 0.3f;

    // ---- Universe and physical scale ----
    int universeMode = 0;           // 0 = painted backdrop, 1 = invented 3D universe, 2 = real 3D universe
    uint32_t universeSeed = 2024u;  // invented universe
    int realPlacement = 0;          // real universe: 0 = Sagittarius A* (galactic centre), 1 = near the Sun
    double bhMassSolar = 1.0e8;     // black hole mass (Gargantua: 100 million suns)
    double timeWarp = 1.0;          // simulated seconds per real second
    bool planetsEnabled = true;     // planets orbiting the black hole
    bool showLabels = true;         // on-screen markers for planets and other targets
    float gasBrightness = 4.0f;     // star-disruption gas: exposure (blackbody luminance relative to 6500 K)
    float gasOpacity = 0.5f;        // star-disruption gas: absorption scale (translucent streams)
    float deepSky = 12.0f;          // 3D universe: long-exposure boost for distant galaxies (1 = true brightness)

    // ---- Camera ----
    float fovDeg = 60.0f;
    float mouseSensitivity = 0.12f;
    bool invertY = false;

    // ---- Flight ----
    int speedMode = 0;              // 0: automatic (follows the distance to the nearest surface), 1: fixed
    float speedScale = 1.0f;        // automatic: multiplier on the speed it picks (scroll wheel)
    double fixedSpeed = 1e9;        // fixed: metres per second, the same everywhere (scroll wheel)
    float turboFactor = 1000.0f;    // turbo (T) multiplies the speed by this
    float travelSpeed = 1.0f;       // Travel list, Go and bookmark flights: 2 = twice as fast

    // ---- Debug / UI ----
    bool showShadowOverlay = false;
    bool showUi = true;
    bool paused = false;

    void applyPreset(int p);
    static const char* presetName(int p);
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    // Every field still at the value `launched` gave it goes back to `loaded`: a launch flag sets a
    // value for that launch only, and it is saved only where the user changed it since (App::run).
    void revertUntouched(const Settings& loaded, const Settings& launched);
    std::string fingerprint() const;       // changes whenever any saved field changes
    std::string traceFingerprint() const;  // only fields that change the traced HDR image
};

// Full-stick flight speed in M per second: automatic follows the distance to the nearest surface (M) times
// the user's multiplier; fixed is the user's own speed (m/s) in units of M (metresPerM); turbo multiplies
// either.
inline double flightSpeedM(const Settings& s, double nearestSurfaceM, double metresPerM, bool turbo) {
    const double base = s.speedMode == 1 ? s.fixedSpeed / metresPerM : 0.35 * s.speedScale * (nearestSurfaceM > 1e-12 ? nearestSurfaceM : 1e-12);
    return turbo ? base * (s.turboFactor < 1.0f ? 1.0 : (s.turboFactor > 1e5f ? 1e5 : s.turboFactor)) : base;
}

}  // namespace bhs
