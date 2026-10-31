#pragma once
#include <glm/glm.hpp>

#include <filesystem>
#include <string>

#include "BlackHoleSystem.h"
#include "Bodies.h"
#include "Camera.h"
#include "DiskTextures.h"
#include "GpuBridge.h"
#include "Renderer.h"
#include "Settings.h"
#include "TdeControls.h"
#include "TdeDirector.h"
#include "GrbControls.h"
#include "grb/GrbDirector.h"
#include "grb/GrbEvent.h"
#include "SnControls.h"
#include "sn/SnDirector.h"
#include "sn/SnEvent.h"
#include "CatalogFetch.h"
#include "UniverseScene.h"
#include "tde/TdeSim.h"
#include "SkyBaker.h"
#include "Ui.h"

struct GLFWwindow;

namespace bhs {

struct Options {
    int width = 1600, height = 900;
    bool fullscreen = false;
    bool wayland = false;
    bool x11 = false;
    bool glDebug = false;
    bool igpu = false;
    bool noUi = false;
    bool forceUi = false;
    bool noCapture = false;
    bool benchmark = false;
    int preset = -1;
    int bookmark = 0;
    int frames = 0;
    int vsync = -1;
    int lensing = -1;
    int diskMode = -1;
    int debugView = 0;
    float doppler = -1.0f;
    float exposure = -1.0f;
    double startTime = 0.0;
    int tdeScenario = 0;        // --tde N: start star-disruption scenario N (1-based)
    int grbScenario = 0;             // --grb N: play neutron-star merger scenario N from the start
    int snScenario = 0;              // --sn N: play supernova scenario N from the start
    double fixedDt = 0.0;       // --dt S: fixed frame time (reproducible animation captures)
    unsigned seed = 0;
    bool hasPos = false, hasTarget = false;
    glm::vec3 pos{0.0f}, target{0.0f};
    double mass = 0.0;          // --mass (solar masses)
    double warp = -1.0;         // --warp (simulated seconds per second)
    std::string gotoTarget;     // --goto NAME: start next to a planet or the black hole
    std::string flyTarget;      // --fly NAME: start a flight to a target at launch
    int universe = -1;          // --universe backdrop|invented|real
    long long universeSeed = -1; // --universe-seed N
    double fovDeg = 0.0;        // --fov DEG (vertical field of view)
    int placement = -1;         // --placement sgra|sun
    std::string screenshot;
    std::string cinematic;
    int captureW = 3840, captureH = 2160, captureSamples = 24;
    std::string shaderDir;
    std::string settingsPath = "settings.ini";
};

class App {
public:
    int run(const Options& opt);

private:
    bool initWindow();
    void selectGpu();
    void setSwapInterval(int interval);
    void shutdown();
    void frame();
    void gatherInput(Camera::Input& in);
    void onKey(int key, int action, int mods);
    void onScroll(double dy);
    void onMouseButton(int button, int action);
    void setMouseCaptured(bool captured);
    int runBenchmark();
    void updateWorld(float dt, const Camera::Input& in);
    void buildBodies();
    const BodyDesc* findBody(const std::string& name) const;
    bool goTo(const std::string& name, bool instant);
    double nearestSurfaceDistance(std::string* nameOut) const;
    static App* self(GLFWwindow* w);

    Options opt_;
    GLFWwindow* window_ = nullptr;
    Settings settings_;
    Settings onDisk_;                    // the settings file's values (the defaults without one)
    Settings launched_;                  // ... after the launch flags: what they set is for that launch only
    Camera camera_;
    Renderer renderer_;
    SkyBaker sky_;
    DiskTextures diskTex_;
    Ui ui_;
    BlackHoleSystem bhSystem_;
    tde::TdeSim tde_;
    UniverseScene universe_;
    int uniModeApplied_ = -1;
    uint32_t uniSeedApplied_ = 0;
    int uniPlacementApplied_ = -1;
    bool uniReload_ = false;             // rebuild the universe although nothing changed (startup, new catalogue files)
    std::string universeError_;
    CatalogFetch catalogFetch_;          // background download of the real galaxy catalogue
    std::filesystem::file_time_type galaxiesStamp_{};   // galaxies.bin's modification time when the universe was built
    std::string fetchLine_;
    bool fetchRequest_ = false;
    void applyUniverseSettings();
    TdeControls tdeCtl_;
    std::vector<GasVolumeRef> gasRefs_;
    GasParticlesRef gasParticles_;     // the disruption's particles, drawn instead of its volumes
    void updateTde(double dtM, double dtReal);
    TdeDirector director_;              // paces, frames and captions a star disruption
    grb::GrbEvent grb_;                 // the neutron-star merger (gamma-ray burst)
    GrbControls grbCtl_;
    GrbDirector grbDirector_;
    GasParticlesRef grbParticles_;
    double grbWarp_ = 0.0;              // the merger's own clock while its auto warp runs (0: none)
    void updateGrb(double dtM, double dtReal);
    sn::SnEvent sn_;                    // the supernova (deep space)
    SnControls snCtl_;
    SnDirector snDirector_;
    GasParticlesRef snParticles_;
    double snWarp_ = 0.0;               // the supernova's own clock while its auto warp runs (0: none)
    bool wasFarEvent_ = false;          // the supernova or the merger played last frame (their shots reach far out)
    bool farEventCam_ = false;          // ... and its director had the camera
    void updateSn(double dtM, double dtReal);
    std::vector<BodyDesc> bodies_;
    std::vector<BodyGpu> bodiesGpu_;
    double simSeconds_ = 0.0;            // simulated coordinate time since start (s)
    std::string followName_;             // body the camera rides along with ("" = none)
    glm::dvec3 followPrevPos_{0.0};
    std::string followLeft_;             // planet the user stopped riding: left alone until the camera is clear of it
    std::string flightTarget_;           // body the current fly-to is heading for
    glm::dvec3 flightPrevPos_{0.0};
    bool wasTransitioning_ = false;
    std::string goToRequest_;            // set by the UI
    std::string rideRequest_;            // set by the UI: a planet to ride along with
    std::string nearestName_;
    bool turbo_ = false;                 // T: 1000x flight speed for crossing interstellar space
    double nearestDist_ = 0.0;
    GLuint targetFbo_ = 0;   // final framebuffer: 0, or the bridge's backbuffer

    std::string gpuName_;
    bool isNvidia_ = false;
    bool nvidiaDriver_ = false;   // NVIDIA kernel module loaded
    enum class GpuPath { Integrated, NvidiaGlx, NvidiaBridge };
    GpuPath gpuPath_ = GpuPath::Integrated;
    GpuBridge bridge_;
    bool mouseCaptured_ = false;
    bool firstMouse_ = true;
    bool automated_ = false;             // --frames / --benchmark: ignore keyboard and mouse
    glm::dvec2 lastCursor_{0.0};
    bool vsyncApplied_ = true;

    double lastTime_ = 0.0;
    double simTime_ = 0.0;
    double diskClock_ = 0.0;
    double fixedDt_ = 0.0;               // --dt: fixed frame time for reproducible captures
    double tdeWarp_ = 0.0;               // the star disruption's own clock while its auto warp runs (0: none)
    double swallowedAt_ = -1.0;          // real time when a star swallowed whole was gone (auto clear)
    double userMass_ = 0.0;              // the user's black hole mass while an event's scenario or the Real
                                         // universe's placement plays its own (0: none)
    bool wasEventActive_ = false;        // an event played last frame (its end gives the black hole back)
    void restoreUserMass();              // gives the black hole back once an event is over or Real is left
    void saveSettings();                 // saves settings with the user's own time warp and black hole mass
    float diskFade_ = 1.0f;              // the steady disk fades out while a star disruption plays
    float debrisFade_ = 0.0f;            // ... and the disk the debris builds forms (a ring spreading inward)
    double reloadTimer_ = 0.0;
    double skyDirtyFor_ = 0.0;
    int lastDebugView_ = 0;
    std::string appRoot_;
    int frameIndex_ = 0;
    int fbW_ = 0, fbH_ = 0;
    std::string lastFingerprint_;
    int debugView_ = 0;

    bool requestScreenshot_ = false;
    bool requestCinematic_ = false;
    bool requestReload_ = false;
    bool requestRebake_ = false;
    bool requestSave_ = false;
    bool quit_ = false;

    float fps_ = 0.0f;
    int fpsFrames_ = 0;
    double fpsTimer_ = 0.0;
};

}  // namespace bhs
