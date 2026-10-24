#pragma once
#include <cstdint>
#include <string>

namespace bhs {

enum DiskMode : int { DISK_NONE = 0, DISK_THIN = 1, DISK_VOLUMETRIC = 2 };
enum TonemapMode : int { TONEMAP_AGX = 0, TONEMAP_ACES = 1, TONEMAP_REINHARD = 2, TONEMAP_NONE = 3 };
enum Preset : int { PRESET_LOW = 0, PRESET_MEDIUM = 1, PRESET_HIGH = 2, PRESET_ULTRA = 3, PRESET_CUSTOM = 4 };

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

    // ---- Camera ----
    float fovDeg = 60.0f;
    float mouseSensitivity = 0.12f;
    bool invertY = false;

    // ---- Debug / UI ----
    bool showShadowOverlay = false;
    bool showUi = true;
    bool paused = false;

    void applyPreset(int p);
    static const char* presetName(int p);
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    std::string fingerprint() const;  // changes whenever any saved field changes
};

}  // namespace bhs
