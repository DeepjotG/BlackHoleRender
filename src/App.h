#pragma once
#include <glm/glm.hpp>

#include <string>

#include "Camera.h"
#include "DiskTextures.h"
#include "Renderer.h"
#include "Settings.h"
#include "SkyBaker.h"
#include "Ui.h"

struct GLFWwindow;

namespace bhs {

struct Options {
    int width = 1600, height = 900;
    bool fullscreen = false;
    bool wayland = false;
    bool x11 = false;
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
    unsigned seed = 0;
    bool hasPos = false, hasTarget = false;
    glm::vec3 pos{0.0f}, target{0.0f};
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
    void shutdown();
    void frame();
    void gatherInput(Camera::Input& in);
    void onKey(int key, int action, int mods);
    void onScroll(double dy);
    void onMouseButton(int button, int action);
    void setMouseCaptured(bool captured);
    int runBenchmark();
    static App* self(GLFWwindow* w);

    Options opt_;
    GLFWwindow* window_ = nullptr;
    Settings settings_;
    Camera camera_;
    Renderer renderer_;
    SkyBaker sky_;
    DiskTextures diskTex_;
    Ui ui_;

    std::string gpuName_;
    bool isNvidia_ = false;
    bool mouseCaptured_ = false;
    bool firstMouse_ = true;
    glm::dvec2 lastCursor_{0.0};
    bool vsyncApplied_ = true;

    double lastTime_ = 0.0;
    double simTime_ = 0.0;
    double reloadTimer_ = 0.0;
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
