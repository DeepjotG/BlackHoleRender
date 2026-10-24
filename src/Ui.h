#pragma once
#include <string>
#include <vector>

struct GLFWwindow;

namespace bhs {

class Camera;
class Renderer;
class BlackHoleSystem;
struct BodyDesc;
struct Settings;
struct TdeControls;
struct UniverseTarget;
namespace tde { struct TdeStats; }

struct UiContext {
    Settings* settings = nullptr;
    Camera* camera = nullptr;
    Renderer* renderer = nullptr;
    const char* gpuName = "";
    bool isNvidia = false;
    bool nvidiaDriverLoaded = false;
    bool bridged = false;
    bool igpuForced = false;
    float bridgeCopyMs = 0.0f;
    float fps = 0.0f;
    double skyBakeMs = 0.0;
    int* debugView = nullptr;
    bool* requestScreenshot = nullptr;
    bool* requestCinematic = nullptr;
    bool* requestReload = nullptr;
    bool* requestRebake = nullptr;
    bool* requestSave = nullptr;
    bool mouseCaptured = false;
    bool skyDirty = false;
    BlackHoleSystem* bhSystem = nullptr;
    const std::vector<BodyDesc>* bodies = nullptr;
    double simSeconds = 0.0;
    std::string* followName = nullptr;    // body the camera rides along with
    std::string* goToRequest = nullptr;   // set to a target name to start a flight
    const char* nearestName = "";
    double nearestDist = 0.0;             // M, to the nearest surface
    TdeControls* tde = nullptr;
    const tde::TdeStats* tdeStats = nullptr;
    bool tdeActive = false;
    const std::vector<UniverseTarget>* uniTargets = nullptr;
    const char* universeStatus = "";
    const char* universeError = "";
    size_t uniStars = 0;
    double simJulianDate = 0.0;
};

class Ui {
public:
    bool init(GLFWwindow* window);
    void shutdown();
    void newFrame(bool mouseCaptured);
    void endFrame();   // when nothing is drawn this frame
    void draw(UiContext& ctx);
    void render();
    bool wantsKeyboard() const;
    bool wantsMouse() const;
    bool anyItemActive() const;

private:
    void drawMarkers(UiContext& ctx);
    bool inited_ = false;
    bool showHelp_ = true;
    char searchBuf_[96] = {0};
};

}  // namespace bhs
