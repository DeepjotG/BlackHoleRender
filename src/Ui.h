#pragma once
#include <string>

struct GLFWwindow;

namespace bhs {

class Camera;
class Renderer;
struct Settings;

struct UiContext {
    Settings* settings = nullptr;
    Camera* camera = nullptr;
    Renderer* renderer = nullptr;
    const char* gpuName = "";
    bool isNvidia = false;
    float fps = 0.0f;
    double skyBakeMs = 0.0;
    int* debugView = nullptr;
    bool* requestScreenshot = nullptr;
    bool* requestCinematic = nullptr;
    bool* requestReload = nullptr;
    bool* requestRebake = nullptr;
    bool* requestSave = nullptr;
    bool mouseCaptured = false;
};

class Ui {
public:
    bool init(GLFWwindow* window);
    void shutdown();
    void newFrame();
    void draw(UiContext& ctx);
    void render();
    bool wantsKeyboard() const;
    bool wantsMouse() const;

private:
    bool inited_ = false;
    bool showHelp_ = true;
};

}  // namespace bhs
