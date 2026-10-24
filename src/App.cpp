#include "App.h"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "Capture.h"
#include "Units.h"
#include "Log.h"

namespace bhs {

namespace {

void glfwErrorCb(int code, const char* desc) { logWarn("GLFW {}: {}", code, desc); }

void APIENTRY glDebugCb(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_PERFORMANCE || type == GL_DEBUG_TYPE_OTHER) return;
    logWarn("GL debug {}: {}", id, msg);
}

}  // namespace

App* App::self(GLFWwindow* w) { return static_cast<App*>(glfwGetWindowUserPointer(w)); }

namespace {
void setOffloadEnv() {
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
}
void clearOffloadEnv() {
    unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    unsetenv("__GLX_VENDOR_LIBRARY_NAME");
    unsetenv("__VK_LAYER_NV_optimus");
}
// Restricting EGL to the NVIDIA vendor makes native Wayland windows fail outright.
void dropNvidiaOnlyEgl() {
    const char* egl = std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES");
    if (egl && std::strstr(egl, "nvidia")) unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
}
// The bridge needs both EGL vendors: Mesa for the Wayland window, NVIDIA for the render device.
// An inherited restriction to one of them (e.g. a Mesa-only power-saving tweak) hides the other.
void allowAllEglVendors() {
    if (std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") || std::getenv("__EGL_VENDOR_LIBRARY_DIRS"))
        logInfo("ignoring __EGL_VENDOR_LIBRARY_FILENAMES/DIRS: the GPU bridge needs the Mesa and NVIDIA EGL vendors");
    unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
    unsetenv("__EGL_VENDOR_LIBRARY_DIRS");
}
}  // namespace

// Chooses the GPU and how its frames reach the screen, before any OpenGL vendor library is
// resolved (glvnd reads the offload variables lazily, on the first GLX call for a screen).
//   NvidiaBridge: native Wayland window on the display GPU, rendering on an NVIDIA EGL device
//                 context (see GpuBridge). Fast on hybrid laptops under Wayland.
//   NvidiaGlx:    PRIME render offload through GLX (X11 sessions, or --x11).
//   Integrated:   whatever GPU drives the display.
void App::selectGpu() {
    std::error_code ec;
    nvidiaDriver_ = std::filesystem::exists("/proc/driver/nvidia/version", ec);
    dropNvidiaOnlyEgl();
    // NVIDIA's driver busy-waits a whole CPU core in glClientWaitSync by default; on a laptop that
    // heat comes out of the GPU's thermal budget. Keep any value the user set.
    setenv("__GL_YIELD", "USLEEP", 0);
    if (opt_.igpu || !nvidiaDriver_) {
        gpuPath_ = GpuPath::Integrated;
        clearOffloadEnv();
        if (opt_.igpu) logInfo("--igpu: using the integrated GPU");
        return;
    }
    const bool haveWayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    if (opt_.x11 || (!haveWayland && !opt_.wayland)) {
        gpuPath_ = GpuPath::NvidiaGlx;
        setOffloadEnv();
        logInfo("NVIDIA driver detected: rendering on the discrete GPU (GLX offload)");
    } else {
        gpuPath_ = GpuPath::NvidiaBridge;
        clearOffloadEnv();   // the window itself belongs to the display GPU
        allowAllEglVendors();
        logInfo("NVIDIA driver detected: rendering on the discrete GPU, shown through a native Wayland window");
    }
}

void App::setSwapInterval(int interval) {
    if (bridge_.active()) bridge_.setSwapInterval(interval);
    else glfwSwapInterval(interval);
}

bool App::initWindow() {
    glfwSetErrorCallback(glfwErrorCb);
    selectGpu();

    // Platform order. GLX offload needs X11 (XWayland); everything else prefers native Wayland,
    // because XWayland's GLX may lack DRI3 and then has no fast hardware path at all.
    const bool haveWayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    std::vector<int> order;
    if (gpuPath_ == GpuPath::NvidiaGlx || opt_.x11 || (!haveWayland && !opt_.wayland))
        order = {GLFW_PLATFORM_X11, GLFW_PLATFORM_WAYLAND};
    else
        order = {GLFW_PLATFORM_WAYLAND, GLFW_PLATFORM_X11};

    for (size_t attempt = 0; attempt < order.size() && !window_; ++attempt) {
        const int platform = order[attempt];
        const char* pname = platform == GLFW_PLATFORM_WAYLAND ? "Wayland" : "X11";
        if (!glfwPlatformSupported(platform)) continue;
        // Switching window system on a fallback also switches how the NVIDIA GPU is reached.
        if (platform == GLFW_PLATFORM_X11 && gpuPath_ == GpuPath::NvidiaBridge) {
            setOffloadEnv();
            gpuPath_ = GpuPath::NvidiaGlx;
        } else if (platform == GLFW_PLATFORM_WAYLAND && gpuPath_ == GpuPath::NvidiaGlx) {
            clearOffloadEnv();
            allowAllEglVendors();
            gpuPath_ = GpuPath::NvidiaBridge;
        }
        glfwInitHint(GLFW_PLATFORM, platform);
        if (!glfwInit()) {
            logWarn("could not initialise the {} window system, trying the next one", pname);
            continue;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#ifndef NDEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#else
        if (opt_.glDebug) glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
        GLFWmonitor* monitor = nullptr;
        int w = opt_.width, h = opt_.height;
        if (opt_.fullscreen) {
            monitor = glfwGetPrimaryMonitor();
            if (const GLFWvidmode* m = monitor ? glfwGetVideoMode(monitor) : nullptr) {
                w = m->width;
                h = m->height;
                glfwWindowHint(GLFW_RED_BITS, m->redBits);
                glfwWindowHint(GLFW_GREEN_BITS, m->greenBits);
                glfwWindowHint(GLFW_BLUE_BITS, m->blueBits);
                glfwWindowHint(GLFW_REFRESH_RATE, m->refreshRate);
            }
        }
        window_ = glfwCreateWindow(w, h, "Black Hole Simulation", monitor, nullptr);
        if (!window_) {
            logWarn("could not open an OpenGL 4.6 window through {}{}", pname,
                    attempt + 1 < order.size() ? ", trying the next window system" : "");
            glfwTerminate();
        }
    }
    if (!window_) {
        logError("no window system could provide an OpenGL 4.6 core context");
        return false;
    }
    glfwMakeContextCurrent(window_);
    const int ver = gladLoadGL(glfwGetProcAddress);
    if (ver == 0) {
        logError("failed to load OpenGL functions");
        return false;
    }
#ifndef NDEBUG
    const bool debugContext = true;
#else
    const bool debugContext = opt_.glDebug;
#endif
    if (gpuPath_ == GpuPath::NvidiaBridge && !bridge_.init(window_, debugContext)) {
        logWarn("NVIDIA rendering bridge unavailable ({}); rendering on the integrated GPU", bridge_.error());
        logWarn("Alternative: ./run.sh --x11 renders on the NVIDIA GPU through XWayland (slow if XWayland lacks DRI3)");
        gpuPath_ = GpuPath::Integrated;
    }
    // From here on the current context is the one that renders (the NVIDIA one when bridged).
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    gpuName_ = renderer ? renderer : "unknown GPU";
    isNvidia_ = gpuName_.find("NVIDIA") != std::string::npos;
    logInfo("OpenGL {} | {}", version ? version : "?", gpuName_);
    GLint glMajor = 0, glMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);
    if (glMajor * 10 + glMinor < 46) {
        logError("OpenGL 4.6 is required, got {}.{}", glMajor, glMinor);
        return false;
    }
    if (bridge_.active()) logInfo("display: native Wayland window, frames copied from {} each frame", gpuName_);
    if (!isNvidia_ && nvidiaDriver_ && !opt_.igpu) {
        logWarn("The NVIDIA driver is loaded but this window renders on {}.", gpuName_);
        logWarn("Check that nvidia-smi lists the RTX 3060; after a driver update, reboot first.");
    } else if (!isNvidia_ && !nvidiaDriver_) {
        logWarn("Not running on the NVIDIA GPU (its kernel module is not loaded). Expect lower frame rates.");
        logWarn("Fix: enroll the driver's Secure Boot key (README.md, 'Enabling the RTX 3060').");
    }
#ifdef NDEBUG
    if (opt_.glDebug) {
#else
    {
#endif
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCb, nullptr);
    }
    vsyncApplied_ = settings_.vsync;
    setSwapInterval(vsyncApplied_ ? 1 : 0);
    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, [](GLFWwindow* win, int key, int, int action, int mods) { self(win)->onKey(key, action, mods); });
    glfwSetScrollCallback(window_, [](GLFWwindow* win, double, double dy) { self(win)->onScroll(dy); });
    glfwSetMouseButtonCallback(window_, [](GLFWwindow* win, int b, int a, int) { self(win)->onMouseButton(b, a); });
    glfwGetFramebufferSize(window_, &fbW_, &fbH_);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    return true;
}

int App::run(const Options& opt) {
    opt_ = opt;
    ShaderProgram::setShaderDir(opt.shaderDir.empty() ? BHS_SHADER_DIR : opt.shaderDir);
    // Resolve settings and screenshots next to the project (build/..), not the launch directory.
    {
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        appRoot_ = ec ? std::filesystem::current_path().string() : exe.parent_path().parent_path().string();
        if (opt_.settingsPath == "settings.ini") opt_.settingsPath = appRoot_ + "/settings.ini";
    }
    const bool loaded = settings_.load(opt_.settingsPath);
    if (loaded) logInfo("loaded settings from {}", opt_.settingsPath);
    const bool automated = opt.frames > 0 || opt.benchmark;
    if (automated) {
        // Deterministic, UI-free rendering for scripted captures.
        settings_ = Settings{};
        settings_.vsync = false;
        settings_.showUi = opt.forceUi;
        settings_.dynamicResolution = false;
    }
    if (opt.preset >= 0) settings_.applyPreset(opt.preset);
    if (opt.seed) settings_.skySeed = opt.seed;
    if (opt.vsync >= 0) settings_.vsync = opt.vsync != 0;
    if (!automated) settings_.showUi = !opt.noUi;   // a hidden panel is never persisted across launches
    if (opt.noUi) settings_.showUi = false;
    if (opt.lensing >= 0) settings_.lensing = opt.lensing != 0;
    if (opt.diskMode >= 0) settings_.diskMode = opt.diskMode;
    if (opt.doppler >= 0.0f) settings_.dopplerStrength = opt.doppler;
    if (opt.exposure > 0.0f) settings_.exposure = opt.exposure;
    if (opt.mass > 0.0) settings_.bhMassSolar = opt.mass;
    if (opt.warp >= 0.0) settings_.timeWarp = opt.warp;
    if (opt.universe >= 0) settings_.universeMode = opt.universe;
    debugView_ = opt.debugView;
    simTime_ = opt.startTime;

    if (!initWindow()) return 1;
    // First interactive run without the NVIDIA driver: start on Medium so it stays smooth. The
    // choice is persisted, so this never overrides a saved preset or --preset.
    if (!isNvidia_ && !loaded && !automated && opt.preset < 0) {
        settings_.applyPreset(PRESET_MEDIUM);
        logWarn("non-NVIDIA GPU and no settings file: starting on the Medium preset (Quality panel to change)");
    }
    if (!ui_.init(window_)) {
        logError("ImGui init failed");
        return 1;
    }
    diskTex_.init();
    sky_.init();
    sky_.bake(settings_);
    if (!renderer_.init()) logError("shader compilation failed; fix the shaders and press F5");
    tdeCtl_.moduleAvailable = tde_.init(262144);
    if (!universe_.init()) logWarn("3D universe shaders failed to compile; only the painted backdrop is available");
    applyUniverseSettings();
    if (!tdeCtl_.moduleAvailable) logInfo("star-disruption simulation not available in this build");

    const auto& bms = Camera::defaultBookmarks();
    if (opt.bookmark >= 1 && opt.bookmark <= static_cast<int>(bms.size()))
        camera_.setPose(bms[opt.bookmark - 1].pos, bms[opt.bookmark - 1].orient);
    if (opt.hasPos) {
        camera_.setPose(glm::dvec3(opt.pos), camera_.orientation());
        camera_.lookAt(opt.hasTarget ? glm::dvec3(opt.target) : glm::dvec3(0.0));
    } else if (opt.hasTarget) {
        camera_.lookAt(glm::dvec3(opt.target));
    }
    buildBodies();
    if (!opt.gotoTarget.empty() && !goTo(opt.gotoTarget, true)) logWarn("--goto: unknown target '{}'", opt.gotoTarget);
    if (!opt.flyTarget.empty()) goToRequest_ = opt.flyTarget;
    if (!automated && !opt.noCapture) setMouseCaptured(true);

    lastTime_ = glfwGetTime();
    lastFingerprint_ = settings_.traceFingerprint();
    lastDebugView_ = debugView_;
    int rc = 0;
    if (opt.benchmark) {
        rc = runBenchmark();
    } else {
        while (!glfwWindowShouldClose(window_) && !quit_) {
            frame();
            if (opt.frames > 0 && frameIndex_ >= opt.frames) break;
        }
        if (!automated) settings_.save(opt_.settingsPath);
    }
    shutdown();
    return rc;
}

void App::shutdown() {
    ui_.shutdown();
    tde_.shutdown();
    universe_.shutdown();
    renderer_.shutdown();
    sky_.shutdown();
    diskTex_.shutdown();
    bridge_.shutdown();
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

void App::setMouseCaptured(bool captured) {
    mouseCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    firstMouse_ = true;
}

void App::onKey(int key, int action, int) {
    if (action != GLFW_PRESS) return;
    if (ui_.wantsKeyboard() && key != GLFW_KEY_ESCAPE && key != GLFW_KEY_TAB) return;
    const auto& bms = Camera::defaultBookmarks();
    switch (key) {
        case GLFW_KEY_ESCAPE:
            if (ui_.wantsKeyboard()) break;               // a text field is active: ImGui cancels the edit
            if (mouseCaptured_) setMouseCaptured(false);  // first Esc releases the mouse
            else quit_ = true;                            // Esc with the mouse already free quits
            break;
        case GLFW_KEY_TAB: setMouseCaptured(!mouseCaptured_); break;
        case GLFW_KEY_H: settings_.showUi = !settings_.showUi; break;
        case GLFW_KEY_F2: requestScreenshot_ = true; break;
        case GLFW_KEY_F3: requestCinematic_ = true; break;
        case GLFW_KEY_F5: requestReload_ = true; break;
        case GLFW_KEY_R: camera_.flyTo(bms[0]); break;
        case GLFW_KEY_1: camera_.flyTo(bms[0]); break;
        case GLFW_KEY_2: camera_.flyTo(bms[1]); break;
        case GLFW_KEY_3: camera_.flyTo(bms[2]); break;
        case GLFW_KEY_4: camera_.flyTo(bms[3]); break;
        case GLFW_KEY_O: camera_.setAutoOrbit(!camera_.autoOrbit()); break;
        case GLFW_KEY_P: settings_.paused = !settings_.paused; break;
        case GLFW_KEY_L: settings_.lensing = !settings_.lensing; break;
        case GLFW_KEY_T: turbo_ = !turbo_; break;
        default: break;
    }
}

void App::onScroll(double dy) {
    if (!mouseCaptured_ && ui_.wantsMouse()) return;
    camera_.baseSpeed = std::clamp(camera_.baseSpeed * static_cast<float>(std::pow(1.5, dy)), 1e-4f, 1e9f);
}

void App::onMouseButton(int button, int action) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !mouseCaptured_ && !ui_.wantsMouse()) setMouseCaptured(true);
}

void App::gatherInput(Camera::Input& in) {
    if (ui_.wantsKeyboard()) return;
    auto down = [&](int k) { return glfwGetKey(window_, k) == GLFW_PRESS; };
    in.move.x = (down(GLFW_KEY_D) ? 1.0f : 0.0f) - (down(GLFW_KEY_A) ? 1.0f : 0.0f);
    in.move.y = (down(GLFW_KEY_SPACE) ? 1.0f : 0.0f) -
                ((down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL) || down(GLFW_KEY_C)) ? 1.0f : 0.0f);
    in.move.z = (down(GLFW_KEY_S) ? 1.0f : 0.0f) - (down(GLFW_KEY_W) ? 1.0f : 0.0f);
    in.roll = (down(GLFW_KEY_E) ? 1.0f : 0.0f) - (down(GLFW_KEY_Q) ? 1.0f : 0.0f);
    in.boost = down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT);
    if (mouseCaptured_) {
        double cx, cy;
        glfwGetCursorPos(window_, &cx, &cy);
        if (firstMouse_) {
            lastCursor_ = {cx, cy};
            firstMouse_ = false;
        }
        const glm::dvec2 d = glm::dvec2(cx, cy) - lastCursor_;
        lastCursor_ = {cx, cy};
        const float sens = settings_.mouseSensitivity * 0.01f;  // radians per pixel
        in.lookDelta = glm::vec2(static_cast<float>(d.x) * sens,
                                 static_cast<float>(d.y) * sens * (settings_.invertY ? -1.0f : 1.0f));
    }
}

void App::frame() {
    glfwPollEvents();
    glfwGetFramebufferSize(window_, &fbW_, &fbH_);
    if (fbW_ == 0 || fbH_ == 0) {
        glfwWaitEventsTimeout(0.1);
        return;
    }
    const double now = glfwGetTime();
    const float dt = static_cast<float>(std::min(now - lastTime_, 0.1));
    lastTime_ = now;

    Camera::Input in;
    gatherInput(in);
    if (!settings_.paused) simTime_ += dt;
    updateWorld(dt, in);

    reloadTimer_ += dt;
    if (reloadTimer_ > 0.5) {
        reloadTimer_ = 0.0;
        renderer_.checkShaderReload();
    }
    if (requestReload_) {
        requestReload_ = false;
        renderer_.reloadShaders();
    }
    // ImGui sliders change the value on every frame of a drag and each bake is a synchronous
    // 1-4 s compute job, so wait until the widget is released and the value has settled.
    const bool skyDirty = sky_.needsRebake(settings_);
    skyDirtyFor_ = skyDirty ? skyDirtyFor_ + dt : 0.0;
    if (requestRebake_ || (skyDirty && skyDirtyFor_ > 0.3 && !ui_.anyItemActive())) {
        requestRebake_ = false;
        skyDirtyFor_ = 0.0;
        sky_.bake(settings_);
        renderer_.resetAccumulation();
    }
    if (requestSave_) {
        requestSave_ = false;
        settings_.save(opt_.settingsPath);
    }
    if (settings_.vsync != vsyncApplied_) {
        vsyncApplied_ = settings_.vsync;
        setSwapInterval(vsyncApplied_ ? 1 : 0);
    }

    const std::string fp = settings_.traceFingerprint();
    const bool settingsChanged = fp != lastFingerprint_ || debugView_ != lastDebugView_;
    lastFingerprint_ = fp;
    lastDebugView_ = debugView_;

    targetFbo_ = bridge_.active() ? bridge_.backbuffer(fbW_, fbH_) : 0;

    FrameInput fi;
    fi.targetFbo = targetFbo_;
    fi.extraGpuMs = bridge_.active() ? bridge_.copyMs() : 0.0f;
    fi.camera = &camera_;
    fi.settings = &settings_;
    fi.bodies = &bodiesGpu_;
    fi.farMode = camera_.radius() > 1e7;
    gasRefs_.clear();
    for (int i = 0; i < tde_.volumeCount() && i < 4; ++i) {
        const tde::GasVolume& gv = tde_.volume(i);
        if (gv.texture) gasRefs_.push_back({gv.texture, gv.center, gv.halfSize, gv.resolution});
    }
    fi.gas = gasRefs_.data();
    fi.gasCount = static_cast<int>(gasRefs_.size());
    fi.universe = universe_.active();
    fi.uniCube = universe_.diffuseCube();
    fi.uniStars = universe_.starBuffer();
    fi.uniTiles = universe_.tileBuffer();
    fi.uniTileGrid = UniverseScene::kTileGrid;
    fi.uniStarScale = settings_.starBrightness * 15.0f;
    fi.time = static_cast<float>(simTime_);
    fi.dt = dt;
    fi.windowW = fbW_;
    fi.windowH = fbH_;
    fi.skyCubemap = sky_.cubemap();
    fi.noise3d = diskTex_.noise3d();
    fi.blackbodyLut = diskTex_.blackbodyLut();
    fi.resetAccumulation = camera_.movedThisFrame() || settingsChanged;
    fi.diskAnimating = !settings_.paused && settings_.diskSpeed > 0.0f && settings_.diskMode != DISK_NONE;
    fi.debugView = debugView_;
    renderer_.render(fi);

    const int cinematicFrame = (opt_.frames == 1) ? 0 : 1;   // one warm-up frame when possible
    if (requestCinematic_ || (!opt_.cinematic.empty() && frameIndex_ == cinematicFrame)) {
        requestCinematic_ = false;
        const std::string path = opt_.cinematic.empty() ? capture::timestampedPath(appRoot_ + "/screenshots", "cinematic") : opt_.cinematic;
        renderer_.captureHighRes(fi, opt_.captureW, opt_.captureH, opt_.captureSamples, path);
        if (!opt_.cinematic.empty()) quit_ = true;
    }

    const bool lastFrame = opt_.frames > 0 && frameIndex_ + 1 >= opt_.frames;
    const bool wantShot = requestScreenshot_ || (lastFrame && !opt_.screenshot.empty());

    // Run the ImGui frame every frame so its input queue drains and the Want* flags stay current;
    // only the drawing is skipped when the panel is hidden.
    ui_.newFrame(mouseCaptured_);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo_);
    glViewport(0, 0, fbW_, fbH_);
    // Screenshots are clean (no panel) unless an automated run asked for the panel with --ui.
    if (settings_.showUi && (!wantShot || opt_.forceUi)) {
        UiContext ctx;
        ctx.settings = &settings_;
        ctx.camera = &camera_;
        ctx.renderer = &renderer_;
        ctx.gpuName = gpuName_.c_str();
        ctx.isNvidia = isNvidia_;
        ctx.nvidiaDriverLoaded = nvidiaDriver_;
        ctx.bridged = bridge_.active();
        ctx.igpuForced = opt_.igpu;
        ctx.bridgeCopyMs = bridge_.active() ? bridge_.copyMs() : 0.0f;
        ctx.fps = fps_;
        ctx.skyBakeMs = sky_.lastBakeMs();
        ctx.debugView = &debugView_;
        ctx.requestScreenshot = &requestScreenshot_;
        ctx.requestCinematic = &requestCinematic_;
        ctx.requestReload = &requestReload_;
        ctx.requestRebake = &requestRebake_;
        ctx.requestSave = &requestSave_;
        ctx.mouseCaptured = mouseCaptured_;
        ctx.skyDirty = skyDirty;
        ctx.bhSystem = &bhSystem_;
        ctx.bodies = &bodies_;
        ctx.simSeconds = simSeconds_;
        ctx.followName = &followName_;
        ctx.goToRequest = &goToRequest_;
        ctx.nearestName = nearestName_.c_str();
        ctx.nearestDist = nearestDist_;
        ctx.turbo = &turbo_;
        ctx.tde = &tdeCtl_;
        ctx.tdeStats = &tde_.stats();
        ctx.tdeActive = tde_.active();
        ctx.uniTargets = &universe_.targets();
        ctx.universeStatus = universe_.status().c_str();
        ctx.universeError = universeError_.c_str();
        ctx.uniStars = universe_.starCount();
        ctx.simJulianDate = universe_.julianDate(simSeconds_);
        ui_.draw(ctx);
        ui_.render();
    } else {
        ui_.endFrame();
    }

    if (wantShot) {
        requestScreenshot_ = false;
        const std::string path = (lastFrame && !opt_.screenshot.empty()) ? opt_.screenshot
                                                                         : capture::timestampedPath(appRoot_ + "/screenshots", "blackhole");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, targetFbo_);
        capture::savePngFromFramebuffer(path, fbW_, fbH_);
    }

    if (bridge_.active()) bridge_.present();
    else glfwSwapBuffers(window_);
    ++frameIndex_;
    ++fpsFrames_;
    fpsTimer_ += dt;
    if (fpsTimer_ >= 0.5) {
        fps_ = static_cast<float>(fpsFrames_ / fpsTimer_);
        fpsFrames_ = 0;
        fpsTimer_ = 0.0;
    }
}

// ---------------------------------------------------------------------------------- world
const BodyDesc* App::findBody(const std::string& name) const {
    for (const BodyDesc& b : bodies_)
        if (b.name == name) return &b;
    return nullptr;
}

double App::nearestSurfaceDistance(std::string* nameOut) const {
    const glm::dvec3 cam = camera_.position();
    double best = glm::length(cam) - 2.0;   // the horizon
    if (nameOut) *nameOut = "black hole";
    if (universe_.active() && universe_.nearestSurfaceM() < best) {
        best = universe_.nearestSurfaceM();
        if (nameOut) *nameOut = universe_.nearestName();
    }
    for (const BodyDesc& b : bodies_) {
        if (!b.collidable) continue;
        const double d = glm::length(cam - b.pos) - b.radius;
        if (d < best) {
            best = d;
            if (nameOut) *nameOut = b.name;
        }
    }
    return best;
}

// Flies to (or, with `instant`, jumps next to) a named target: a body or the black hole.
bool App::goTo(const std::string& name, bool instant) {
    glm::dvec3 target(0.0);
    double dist = 30.0;
    glm::dvec3 fromDir(0.0);
    const BodyDesc* b = nullptr;
    if (name == "black hole" || name == "Gargantua" || name == "Sagittarius A*") {
        fromDir = glm::normalize(glm::dvec3(0.0, 0.1, 1.0));
    } else {
        b = findBody(name);
        if (!b) {
            // Stars, galaxies and places of the 3D universe.
            for (const UniverseTarget& t : universe_.targets()) {
                if (t.name != name) continue;
                const double viewDist = t.kind == 0 ? t.radius * 8.0 : (t.kind == 1 ? t.radius * 2.2 : t.radius * 2.0);
                const glm::dvec3 bearing = glm::length(camera_.position() - t.pos) > 0.0 ? glm::normalize(camera_.position() - t.pos)
                                                                                          : glm::dvec3(0, 0, 1);
                if (instant) {
                    camera_.setPose(t.pos + bearing * viewDist, camera_.orientation());
                    camera_.lookAt(t.pos);
                } else {
                    followName_.clear();
                    flightTarget_.clear();
                    camera_.flyToTarget(t.pos, viewDist, bearing);
                }
                return true;
            }
            return false;
        }
        target = b->pos;
        dist = b->radius * (b->kind == BodyKind::Star ? 6.0 : 4.5);
        // Side view from behind the planet's motion: the terminator crosses the disc and the
        // black hole sits beside it in the frame.
        const glm::dvec3 out = glm::normalize(b->pos);
        const glm::dvec3 tang = glm::length(b->vel) > 0.0 ? glm::normalize(b->vel) : glm::dvec3(0, 0, 1);
        fromDir = glm::normalize(out * 0.15 - tang * 0.9 + glm::dvec3(0.0, 0.3, 0.0));
    }
    if (instant) {
        const glm::dvec3 pos = target + fromDir * dist;
        camera_.setPose(pos, camera_.orientation());
        camera_.lookAt(target);
        if (b) {
            followName_ = b->name;
            followPrevPos_ = b->pos;
        }
        return true;
    }
    followName_.clear();
    camera_.flyToTarget(target, dist, fromDir);
    flightTarget_ = b ? b->name : std::string();
    flightPrevPos_ = target;
    return true;
}

void App::buildBodies() {
    bodies_.clear();
    const double mass = settings_.bhMassSolar;
    const double mPerM = units::metersPerM(mass);
    const double sPerM = units::secondsPerM(mass);
    if (settings_.planetsEnabled) {
        for (const Planet& p : bhSystem_.planets) {
            BodyDesc b;
            b.name = p.name;
            b.kind = BodyKind::Planet;
            b.pos = p.pos;
            b.vel = p.vel;
            b.radius = BlackHoleSystem::planetRadiusM(p, mass);
            b.look = p.look;
            b.seed = p.seed;
            b.tint = p.tint;
            b.atmosphere = p.atmosphere;
            b.axis = glm::vec3(p.orbitNormal);
            b.rotation = static_cast<float>(p.phase);   // tidally locked: the same face looks at the hole
            const double r = glm::length(p.pos);
            b.lightDir = glm::vec3(-p.pos / std::max(r, 1e-9));
            const bool diskOn = settings_.diskMode != DISK_NONE;
            const double fall = std::pow(6.5 / std::max(r, 6.5), 0.6);
            b.lightIntensity = diskOn ? static_cast<float>(3.0 * fall * settings_.diskBrightness / 5.5) : 0.02f;
            b.lightColor = glm::vec3(1.0f, 0.83f, 0.64f);
            const double period = BlackHoleSystem::orbitalPeriodM(p.orbitRadiusM) * sPerM;
            const double dil = BlackHoleSystem::timeDilationCircularOrbit(p.orbitRadiusM);
            b.info = p.name + ": orbit " + units::formatDistance(p.orbitRadiusM * mPerM) + ", period " + units::formatDuration(period) +
                     ", 1 hour there = " + units::formatDuration(3600.0 * dil) + " far away";
            bodies_.push_back(b);
        }
    }
    if (tde_.starIntact()) {
        BodyDesc b;
        b.name = "Star";
        b.kind = BodyKind::Star;
        b.pos = tde_.starPosition();
        b.vel = tde_.starVelocity();
        b.radius = tde_.starRadiusM();
        b.teff = static_cast<float>(tde_.starTeff());
        b.lightIntensity = 6.0f;
        b.seed = 5;
        b.info = "The star on its way in";
        bodies_.push_back(b);
    }
    universe_.update(camera_.position(), simSeconds_, settings_, bodies_);
}

void App::applyUniverseSettings() {
    const int mode = settings_.universeMode;
    if (mode == uniModeApplied_ && (mode != 1 || settings_.universeSeed == uniSeedApplied_) &&
        (mode != 2 || settings_.realPlacement == uniPlacementApplied_))
        return;
    const bool enteringReal = mode == 2 && uniModeApplied_ != 2;
    uniModeApplied_ = mode;
    uniSeedApplied_ = settings_.universeSeed;
    uniPlacementApplied_ = settings_.realPlacement;
    universeError_.clear();
    if (!universe_.configure(mode, settings_.universeSeed, settings_.realPlacement, appRoot_ + "/assets/real", &universeError_)) {
        logWarn("universe: {}", universeError_);
        return;
    }
    if (enteringReal && universe_.content().suggestedMassSolar > 0.0) settings_.bhMassSolar = universe_.content().suggestedMassSolar;
    renderer_.resetAccumulation();
}

void App::updateTde(double dtM) {
    if (tdeCtl_.clearRequested) {
        tdeCtl_.clearRequested = false;
        tde_.clear();
    }
    if (tdeCtl_.spawnRequested) {
        tdeCtl_.spawnRequested = false;
        tde::StarParams sp;
        sp.massSolar = tdeCtl_.massSolar;
        sp.radiusSolar = tdeCtl_.radiusSolar;
        sp.teff = tdeCtl_.teff;
        sp.polytropeN = tdeCtl_.polytropeN;
        tde::OrbitParams op;
        op.beta = tdeCtl_.beta;
        op.inclinationDeg = tdeCtl_.inclinationDeg;
        op.longitudeDeg = tdeCtl_.longitudeDeg;
        op.retrograde = tdeCtl_.retrograde;
        op.startDistanceRt = 3.0;
        tde_.spawn(sp, op, settings_.bhMassSolar, tdeCtl_.particles);
        tdeCtl_.watchRequested = true;
        logInfo("spawned a {} M_sun, {} R_sun star (beta {})", sp.massSolar, sp.radiusSolar, op.beta);
    }
    if (tdeCtl_.watchRequested) {
        tdeCtl_.watchRequested = false;
        const tde::TdeStats& st = tde_.stats();
        const double rp = std::max(st.pericenterM, 4.0);
        const double D = std::max(30.0, 2.8 * rp);
        const double L = glm::radians(tdeCtl_.longitudeDeg), inc = glm::radians(tdeCtl_.inclinationDeg);
        const glm::dvec3 peri(std::cos(L), 0.0, -std::sin(L));
        const glm::dvec3 normal = glm::normalize(glm::dvec3(0.0, std::cos(inc), 0.0) + glm::cross(peri, glm::dvec3(0, 1, 0)) * std::sin(inc));
        const glm::dvec3 from = glm::normalize(normal * std::sin(glm::radians(35.0)) - peri * std::cos(glm::radians(35.0)));
        followName_.clear();
        camera_.flyToTarget(glm::dvec3(0.0), D, from);
    }
    if (!tde_.active()) return;
    if (tdeCtl_.autoWarp) {
        // Keep the event moving at a watchable pace: about 1/15 of the local dynamical time per
        // second while the star falls in, growing with the age of the debris afterwards.
        const tde::TdeStats& st = tde_.stats();
        const double r = tde_.starIntact() ? glm::length(tde_.starPosition()) : std::max(st.pericenterM, 6.0);
        const double tdyn = std::pow(std::max(r, 3.0), 1.5);
        const double perSecondM = std::max(tdyn / 15.0, (st.released ? 0.12 * st.timeM : 0.0));
        settings_.timeWarp = std::clamp(perSecondM * units::secondsPerM(settings_.bhMassSolar), 1e-3, 1e11);
    }
    tde_.step(dtM);
}

void App::updateWorld(float dt, const Camera::Input& in) {
    applyUniverseSettings();
    // Simulated time: the warp sets how many seconds pass per real second.
    const double dtSim = settings_.paused ? 0.0 : settings_.timeWarp * static_cast<double>(dt);
    simSeconds_ += dtSim;
    const double dtM = dtSim / units::secondsPerM(settings_.bhMassSolar);
    bhSystem_.update(dtM);
    updateTde(dtM);
    buildBodies();

    // Ride along with the planet we are next to (they orbit at a large fraction of c).
    if (!followName_.empty()) {
        if (const BodyDesc* b = findBody(followName_)) {
            camera_.shift(b->pos - followPrevPos_);
            followPrevPos_ = b->pos;
            if (glm::length(camera_.position() - b->pos) > 400.0 * b->radius) followName_.clear();
        } else {
            followName_.clear();
        }
    }
    if (camera_.isTransitioning() && !flightTarget_.empty()) {
        if (const BodyDesc* b = findBody(flightTarget_)) {
            camera_.retargetFlight(b->pos - flightPrevPos_);
            flightPrevPos_ = b->pos;
        }
    }
    if (wasTransitioning_ && !camera_.isTransitioning() && !flightTarget_.empty()) {
        if (const BodyDesc* b = findBody(flightTarget_)) {
            followName_ = b->name;
            followPrevPos_ = b->pos;
        }
        flightTarget_.clear();
    }
    if (followName_.empty() && !camera_.isTransitioning()) {
        for (const BodyDesc& b : bodies_) {
            if (b.kind == BodyKind::Planet && glm::length(camera_.position() - b.pos) < 60.0 * b.radius) {
                followName_ = b.name;
                followPrevPos_ = b.pos;
                break;
            }
        }
    }

    // Flight speed follows the distance to the nearest surface (horizon or planet).
    nearestDist_ = nearestSurfaceDistance(&nearestName_);
    const double speed = 0.35 * camera_.baseSpeed * std::max(nearestDist_, 1e-12) * (turbo_ ? 1000.0 : 1.0);
    camera_.maxRadius = settings_.universeMode == 0 ? 20000.0 : 1e22;
    camera_.update(dt, in, speed);
    wasTransitioning_ = camera_.isTransitioning();
    for (const BodyDesc& b : bodies_)
        if (b.collidable) camera_.pushOutOf(b.pos, b.radius * 1.0003);
    if (!goToRequest_.empty()) {
        if (!goTo(goToRequest_, false)) logWarn("unknown travel target '{}'", goToRequest_);
        goToRequest_.clear();
    }
    // Camera-relative body positions must match the camera position used for this frame.
    bodiesGpu_ = packBodies(bodies_, camera_.position(), 0.25 * 0.0006, 16);
    universe_.prepareRender(camera_.position());
}

int App::runBenchmark() {
    settings_.showUi = false;
    settings_.accumulate = false;
    settings_.dynamicResolution = false;
    struct Row {
        const char* name;
        float gpuMs, frameMs;
        int iw, ih;
    };
    std::vector<Row> rows;
    logInfo("benchmark at {}x{} on {}", fbW_, fbH_, gpuName_);
    for (int p = PRESET_LOW; p <= PRESET_ULTRA; ++p) {
        settings_.applyPreset(p);
        if (sky_.needsRebake(settings_)) sky_.bake(settings_);
        for (int i = 0; i < 8; ++i) frame();
        const double t0 = glfwGetTime();
        float gpuSum = 0.0f;
        int n = 0;
        const int N = 40;
        for (int i = 0; i < N; ++i) {
            frame();
            if (renderer_.gpuMs() > 0.0f) {
                gpuSum += renderer_.gpuMs();
                ++n;
            }
        }
        const double frameMs = (glfwGetTime() - t0) * 1000.0 / N;
        const glm::ivec2 isz = renderer_.internalSize();
        rows.push_back({Settings::presetName(p), n ? gpuSum / n : 0.0f, static_cast<float>(frameMs), isz.x, isz.y});
        if (glfwWindowShouldClose(window_) || quit_) break;
    }
    std::printf("\n%-8s %-12s %10s %10s %8s\n", "Preset", "Internal", "GPU ms", "Frame ms", "FPS");
    for (const Row& r : rows)
        std::printf("%-8s %5dx%-6d %10.2f %10.2f %8.1f\n", r.name, r.iw, r.ih, r.gpuMs, r.frameMs,
                    1000.0f / std::max(r.frameMs, 0.01f));
    std::fflush(stdout);
    return 0;
}

}  // namespace bhs
