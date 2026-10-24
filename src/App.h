#pragma once
#include <glm/glm.hpp>

#include <string>

#include "BlackHoleSystem.h"
#include "Bodies.h"
#include "Camera.h"
#include "DiskTextures.h"
#include "GpuBridge.h"
#include "Renderer.h"
#include "Settings.h"
#include "TdeControls.h"
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
    std::string universeError_;
    CatalogFetch catalogFetch_;          // background download of the real galaxy catalogue
    std::string fetchLine_;
    bool fetchRequest_ = false;
    void applyUniverseSettings();
    TdeControls tdeCtl_;
    std::vector<GasVolumeRef> gasRefs_;
    void updateTde(double dtM);
    std::vector<BodyDesc> bodies_;
    std::vector<BodyGpu> bodiesGpu_;
    double simSeconds_ = 0.0;            // simulated coordinate time since start (s)
    std::string followName_;             // body the camera rides along with ("" = none)
    glm::dvec3 followPrevPos_{0.0};
    std::string flightTarget_;           // body the current fly-to is heading for
    glm::dvec3 flightPrevPos_{0.0};
    bool wasTransitioning_ = false;
    std::string goToRequest_;            // set by the UI
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
    float diskFade_ = 1.0f;              // the steady disk fades out while a star disruption plays
    int watchPhase_ = 0;                 // star disruption camera: 0 off, 1 riding with the star, 2 pulled back
    glm::dvec3 watchPrev_{0.0};
    glm::dvec3 watchOffset_{0.0};        // camera position relative to the star while riding along
    glm::dvec3 watchNormal_{0.0, 1.0, 0.0};   // the star's orbit normal
    double watchLook_ = 0.15;           // how far from the debris towards the hole the camera aims (0..1)
    double watchPeriM_ = 0.0;             // disk flow clock in M, wrapped to the cross-fade period
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
