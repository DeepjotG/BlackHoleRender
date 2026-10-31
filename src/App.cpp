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
    automated_ = automated;
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
    if (opt.universeSeed >= 0) settings_.universeSeed = static_cast<uint32_t>(opt.universeSeed);
    if (opt.fovDeg > 0.0) settings_.fovDeg = static_cast<float>(opt.fovDeg);
    if (opt.placement >= 0) settings_.realPlacement = opt.placement;
    debugView_ = opt.debugView;
    simTime_ = opt.startTime;
    diskClock_ = std::fmod(opt.startTime * settings_.diskSpeed * kDiskFlowRate, 60.0);

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
    if (opt.tdeScenario >= 1 && opt.tdeScenario <= kTdeScenarioCount) {
        if (tdeCtl_.moduleAvailable) {
            settings_.bhMassSolar = tdeCtl_.applyScenario(opt.tdeScenario - 1);
            tdeCtl_.spawnRequested = true;
        } else {
            logWarn("--tde ignored: the star-disruption simulation is not in this build");
        }
    }
    fixedDt_ = opt.fixedDt;

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
        if (!automated) saveSettings();
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
    if (automated_) return;   // captures must not react to whatever happens on the desktop
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
    if (automated_) return;   // captures must not react to whatever happens on the desktop
    if (!mouseCaptured_ && ui_.wantsMouse()) return;
    camera_.baseSpeed = std::clamp(camera_.baseSpeed * static_cast<float>(std::pow(1.5, dy)), 1e-4f, 1e9f);
}

void App::onMouseButton(int button, int action) {
    if (automated_) return;   // captures must not react to whatever happens on the desktop
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !mouseCaptured_ && !ui_.wantsMouse()) setMouseCaptured(true);
}

void App::gatherInput(Camera::Input& in) {
    if (automated_) return;   // captures must not react to whatever happens on the desktop
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
    const float dt = fixedDt_ > 0.0 ? static_cast<float>(fixedDt_) : static_cast<float>(std::min(now - lastTime_, 0.1));
    lastTime_ = now;

    Camera::Input in;
    gatherInput(in);
    if (!settings_.paused) {
        simTime_ += dt;
        // Disk flow: kDiskFlowRate M of orbital motion per second at speed 1 (Settings.h).
        constexpr double kDiskClockPeriod = 60.0;   // the disk shader's cross-fade period
        diskClock_ = std::fmod(diskClock_ + static_cast<double>(dt) * settings_.diskSpeed * kDiskFlowRate, kDiskClockPeriod);
    }
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
        saveSettings();
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
    static const bool noGas = std::getenv("BHS_NO_GAS") != nullptr;   // debug: simulate but do not draw the gas
    if (noGas) gasRefs_.clear();
    // The gas is drawn from the particles themselves (sharp at any distance); the volumes stay as the
    // fallback when the particle shaders are unavailable, or on request (BHS_GAS_VOLUMES, comparisons).
    static const bool volumes = std::getenv("BHS_GAS_VOLUMES") != nullptr;
    const tde::TdeSim::ParticleView pv = tde_.particles();
    if (pv.count > 0 && !noGas && !volumes && renderer_.gasParticlesOk()) {
        gasParticles_ = {pv.pos, pv.vel, pv.aux, pv.params, pv.paramsOffset, pv.paramsSize, pv.count,
                         static_cast<float>(tde_.starRadiusM()), static_cast<float>(director_.nearRadiusM()), 1.0f,
                         tde_.stats().timeM, static_cast<float>(director_.farRadiusM()), static_cast<float>(director_.returnFraction())};
        fi.particles = &gasParticles_;
        gasRefs_.clear();
    }
    fi.gas = gasRefs_.data();
    fi.gasCount = static_cast<int>(gasRefs_.size());
    // 8 star radii: longer lengths make the coarse grids' smeared copy of the debris glow and absorb, which
    // hides the torn star behind a fog (BHS_GAS_REFLEN overrides, in star radii).
    static const double refRadii = std::getenv("BHS_GAS_REFLEN") ? std::atof(std::getenv("BHS_GAS_REFLEN")) : 8.0;
    fi.gasRefLen = static_cast<float>(std::max(refRadii * tde_.starRadiusM(), 1e-6));
    gasParticles_.refLen = fi.gasRefLen;
    if (std::getenv("BHS_DEBUG_TDE") && !gasRefs_.empty()) {
        static int n = 0;
        if (++n % 30 == 0) {
            const glm::dvec3 fwd = -glm::dvec3(camera_.basis()[2]);
            for (size_t i = 0; i < gasRefs_.size(); ++i) {
                const glm::dvec3 rel = glm::dvec3(gasRefs_[i].center) - camera_.position();
                logInfo("    render: volume {} centre {:.1f} deg off axis, {:.3g} M away, half-size {:.3g} M", i,
                        glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(rel), fwd), -1.0, 1.0))), glm::length(rel), gasRefs_[i].halfSize);
            }
        }
    }
    fi.universe = universe_.active();
    fi.uniCube = universe_.diffuseCube();
    fi.uniStars = universe_.starBuffer();
    fi.uniTiles = universe_.tileBuffer();
    fi.uniTileGrid = UniverseScene::kTileGrid;
    fi.uniStarScale = settings_.starBrightness * 15.0f;
    fi.uniSkyScale = universe_.skyExposure();
    fi.time = static_cast<float>(std::fmod(simTime_, 3600.0));
    fi.diskTime = static_cast<float>(diskClock_);
    fi.dt = dt;
    fi.windowW = fbW_;
    fi.windowH = fbH_;
    fi.skyCubemap = sky_.cubemap();
    fi.noise3d = diskTex_.noise3d();
    fi.blackbodyLut = diskTex_.blackbodyLut();
    fi.resetAccumulation = camera_.movedThisFrame() || settingsChanged;
    fi.diskAnimating = !settings_.paused && settings_.diskSpeed > 0.0f && settings_.diskMode != DISK_NONE;
    // A real disruption happens around a quiet black hole and builds its own disk from the debris: the
    // steady disk fades out (over about two seconds) while an event plays, the debris disk fades in as the
    // gas comes back, and after the event the steady disk returns once the debris disk has faded.
    const bool eventDisk = tde_.active() && !tdeCtl_.keepDisk;
    debrisFade_ += ((eventDisk ? director_.diskPresence() : 0.0f) - debrisFade_) * std::min(1.0f, dt * 1.5f);
    const float fadeTarget = eventDisk || debrisFade_ > 0.02f ? 0.0f : 1.0f;
    diskFade_ += (fadeTarget - diskFade_) * std::min(1.0f, dt * 1.5f);
    fi.diskFade = diskFade_;
    if (diskFade_ < 0.01f && debrisFade_ > 0.001f) {
        fi.diskFade = debrisFade_;
        fi.debrisDisk = true;
        fi.debrisRing = static_cast<float>(director_.ringRadiusM());
        fi.diskAnimating = !settings_.paused;
    }
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
    // Test hook: BHS_SHOTS="frame,frame,..." also saves screenshots at those frames, next to --screenshot
    // (shots/a.png -> shots/a_f300.png), so one run can film a whole event.
    std::string hookShot;
    if (const char* shots = std::getenv("BHS_SHOTS"); shots && !opt_.screenshot.empty()) {
        for (const char* q = shots; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr) {
            if (std::atoi(q) != frameIndex_) continue;
            const size_t dot = opt_.screenshot.rfind('.');
            const std::string stem = dot == std::string::npos ? opt_.screenshot : opt_.screenshot.substr(0, dot);
            hookShot = stem + "_f" + std::to_string(frameIndex_) + ".png";
        }
    }
    const bool wantShot = requestScreenshot_ || (lastFrame && !opt_.screenshot.empty()) || !hookShot.empty();
    // Test hook: BHS_DUMP_PARTICLES (with BHS_SHOTS) also writes the gas particles at those frames:
    // int32 count, then count vec4s each of position, velocity and aux, then the parameter block.
    if (!hookShot.empty() && std::getenv("BHS_DUMP_PARTICLES")) {
        const tde::TdeSim::ParticleView pv = tde_.particles();
        if (pv.count > 0) {
            std::vector<glm::vec4> buf(static_cast<size_t>(pv.count));
            std::vector<unsigned char> params(static_cast<size_t>(pv.paramsSize));
            const std::string path = hookShot.substr(0, hookShot.size() - 4) + ".particles";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(&pv.count, sizeof(int), 1, f);
                for (GLuint b : {pv.pos, pv.vel, pv.aux}) {
                    glGetNamedBufferSubData(b, 0, static_cast<GLsizeiptr>(buf.size() * sizeof(glm::vec4)), buf.data());
                    std::fwrite(buf.data(), sizeof(glm::vec4), buf.size(), f);
                }
                glGetNamedBufferSubData(pv.params, pv.paramsOffset, pv.paramsSize, params.data());
                std::fwrite(params.data(), 1, params.size(), f);
                std::fclose(f);
            }
        }
    }

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
        ctx.tdeTitle = director_.title();
        ctx.tdeCaption = director_.caption();
        ctx.tdeFade = director_.fade();
        ctx.tdeWarp = tdeWarp_ > 0.0 ? tdeWarp_ : settings_.timeWarp;
        ctx.diskHidden = tde_.active() && !tdeCtl_.keepDisk;
        ctx.uniTargets = &universe_.targets();
        ctx.universeStatus = universe_.status().c_str();
        ctx.universeError = universeError_.c_str();
        ctx.galaxiesMissing = settings_.universeMode == 2 && universe_.active() && universe_.content().galaxies.empty();
        ctx.fetchState = static_cast<int>(catalogFetch_.state());
        fetchLine_ = catalogFetch_.lastLine();
        ctx.fetchLine = fetchLine_.c_str();
        ctx.fetchRequest = &fetchRequest_;
        ctx.uniStars = universe_.starCount();
        ctx.simJulianDate = universe_.julianDate(simSeconds_);
        ui_.draw(ctx);
        ui_.render();
    } else {
        ui_.endFrame();
    }

    if (wantShot) {
        requestScreenshot_ = false;
        const std::string path = !hookShot.empty()                       ? hookShot
                                 : (lastFrame && !opt_.screenshot.empty()) ? opt_.screenshot
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
    if (name == "nearest star") {
        glm::dvec3 p;
        double r;
        std::string n;
        if (!universe_.active() || !universe_.nearestStar(p, r, n)) return false;
        const glm::dvec3 bearing = glm::normalize(camera_.position() - p + glm::dvec3(1e-30));
        const double viewDist = r * 8.0;
        followName_.clear();
        flightTarget_.clear();
        if (instant) {
            camera_.setPose(p + bearing * viewDist, camera_.orientation());
            camera_.lookAt(p);
        } else {
            camera_.flyToTarget(p, viewDist, bearing);
        }
        return true;
    }
    if (name == "black hole" || name == "Gargantua" || name == "Sagittarius A*") {
        fromDir = glm::normalize(glm::dvec3(0.0, 0.1, 1.0));
    } else {
        b = findBody(name);
        if (!b) {
            // Stars, galaxies and places of the 3D universe.
            for (const UniverseTarget& t : universe_.targets()) {
                if (t.name != name) continue;
                const double viewDist = t.kind == 0 ? t.radius * 8.0 : (t.kind == 1 ? t.radius * 2.2 : t.radius * 2.0);
                glm::dvec3 bearing = glm::length(camera_.position() - t.pos) > 0.0 ? glm::normalize(camera_.position() - t.pos)
                                                                                    : glm::dvec3(0, 0, 1);
                if (glm::length(t.axis) > 0.5) {
                    // Arrive above the disk (on the side we come from) so the spiral structure shows.
                    static const double elevDeg = std::getenv("BHS_VIEW_ELEV") ? std::atof(std::getenv("BHS_VIEW_ELEV")) : 38.0;
                    const double up = glm::dot(bearing, t.axis) >= 0.0 ? 1.0 : -1.0;
                    glm::dvec3 side = bearing - glm::dot(bearing, t.axis) * t.axis;
                    if (glm::length(side) < 1e-6) side = glm::cross(t.axis, std::abs(t.axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
                    const double e = glm::radians(elevDeg);
                    bearing = glm::normalize(std::cos(e) * glm::normalize(side) + std::sin(e) * up * t.axis);
                }
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
        b.lightIntensity = settings_.gasBrightness;   // same brightness law as its gas (see bodies.glsl)
        b.atmosphere = -1.0f;
        b.seed = 5;
        b.info = "The star on its way in";
        bodies_.push_back(b);
    }
    universe_.update(camera_.position(), simSeconds_, settings_, bodies_);
}

void App::saveSettings() {
    // settings_.timeWarp is always the user's own warp (a star disruption runs on its own clock).
    settings_.save(opt_.settingsPath);
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

void App::updateTde(double dtM, double dtReal) {
    if (tdeCtl_.clearRequested) {
        tdeCtl_.clearRequested = false;
        tde_.clear();
    }
    if (tdeCtl_.spawnRequested) {
        tdeCtl_.spawnRequested = false;
        // A selected scenario is played exactly as described, including its black hole.
        if (tdeCtl_.scenario >= 0 && tdeCtl_.scenario < kTdeScenarioCount) settings_.bhMassSolar = tdeCtl_.applyScenario(tdeCtl_.scenario);
        swallowedAt_ = -1.0;
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
        renderer_.resetGasParticleStats();
        director_.start(tde_.starPosition(), tde_.starVelocity());
        tdeCtl_.watchRequested = true;
        logInfo("spawned a {} M_sun, {} R_sun star (beta {})", sp.massSolar, sp.radiusSolar, op.beta);
    }
    if (tdeCtl_.watchRequested) {
        tdeCtl_.watchRequested = false;
        followName_.clear();
        flightTarget_.clear();
        director_.engageCamera();
    }
    if (!tde_.active()) {
        tdeWarp_ = 0.0;
        director_.stop();
        return;
    }
    // A star swallowed whole leaves nothing to watch: end the event a few seconds after it is gone, so the
    // steady disk comes back.
    if (tde_.stats().swallowedWhole && tde_.stats().accretedFraction > 0.99) {
        const double now = glfwGetTime();
        if (swallowedAt_ < 0.0) swallowedAt_ = now;
        if (now - swallowedAt_ > 4.0) {
            tde_.clear();
            director_.stop();
            swallowedAt_ = -1.0;
            tdeWarp_ = 0.0;
            return;
        }
    }
    // The director paces the event's own clock (the planets, the sky and the saved settings keep the
    // user's time warp), frames it and decides how far the debris disk has formed.
    const tde::TdeStats& st = tde_.stats();
    TdeDirector::Input din;
    din.dt = dtReal;
    din.timeM = st.timeM;
    din.active = true;
    din.intact = tde_.starIntact();
    din.released = st.released;
    din.swallowed = st.swallowedWhole;
    din.pericenterM = st.pericenterM;
    din.starRadiusM = tde_.starRadiusM();
    din.pos = tde_.starPosition();
    din.vel = tde_.starVelocity();
    din.accretedFraction = st.accretedFraction;
    din.particles = tdeCtl_.particles;
    din.gas = &renderer_.gasParticleStats();
    director_.update(din);
    tdeWarp_ = tdeCtl_.autoWarp ? std::clamp(director_.pace() * units::secondsPerM(settings_.bhMassSolar), 1e-3, 1e11) : 0.0;
    tde_.step(dtM);
}

void App::updateWorld(float dt, const Camera::Input& in) {
    // Test hook: BHS_SWITCH_UNIVERSE="frame:mode[,frame:mode...]" picks a universe mid-run, exactly as the
    // Universe menu does (mode 0 backdrop, 1 invented, 2 real).
    if (const char* sw = std::getenv("BHS_SWITCH_UNIVERSE")) {
        for (const char* q = sw; *q;) {
            int f = 0, m = 0;
            if (std::sscanf(q, "%d:%d", &f, &m) == 2 && f == frameIndex_) {
                logInfo("test hook: switching to universe mode {} at frame {}", m, f);
                settings_.universeMode = m;
            }
            const char* comma = std::strchr(q, ',');
            if (!comma) break;
            q = comma + 1;
        }
    }
    // Test hook: BHS_SPAWN_AT=frame presses "Spawn star" with whatever the panel currently holds.
    if (const char* sp = std::getenv("BHS_SPAWN_AT"))
        if (std::atoi(sp) == frameIndex_) tdeCtl_.spawnRequested = true;
    if (const char* cl = std::getenv("BHS_CLEAR_AT"))   // ... and BHS_CLEAR_AT=frame presses "Clear"
        if (std::atoi(cl) == frameIndex_) tdeCtl_.clearRequested = true;
    static bool fetchAuto = std::getenv("BHS_FETCH_CATALOG") != nullptr;   // test hook: press the button at start
    if (fetchAuto && settings_.universeMode == 2) {
        fetchAuto = false;
        fetchRequest_ = true;
    }
    if (fetchRequest_) {
        fetchRequest_ = false;
        catalogFetch_.start(appRoot_);
        logInfo("downloading the real galaxy catalogue in the background");
    }
    if (catalogFetch_.takeFinished()) {   // new catalogue files: reload the real universe
        logInfo("real galaxy catalogue downloaded; reloading the universe");
        uniModeApplied_ = -1;
    }
    applyUniverseSettings();
    // Simulated time: the warp sets how many seconds pass per real second.
    // Two clocks: the world (planets, sky, date) runs at the user's warp; a star disruption may run on
    // its own, faster clock (auto warp) so the event stays watchable without the planets racing around.
    const double sPerMNow = units::secondsPerM(settings_.bhMassSolar);
    const double dtSim = settings_.paused ? 0.0 : settings_.timeWarp * static_cast<double>(dt);
    const double dtEvent = settings_.paused ? 0.0 : (tdeWarp_ > 0.0 ? tdeWarp_ : settings_.timeWarp) * static_cast<double>(dt);
    simSeconds_ += dtSim;
    bhSystem_.update(dtSim / sPerMNow);
    updateTde(dtEvent / sPerMNow, dt);
    buildBodies();

    if (tde_.active() && std::getenv("BHS_DEBUG_TDE")) {
        static int frameNo = 0;
        if (++frameNo % 30 == 0) {
            const tde::TdeStats& st = tde_.stats();
            const GasParticleStats& gs = renderer_.gasParticleStats();
            logInfo("    director: stage {} pace {:.3g} M/s disk {:.2f} camera {} | gas: in play {} near {} box [{:.3g} {:.3g} {:.3g}]..[{:.3g} {:.3g} {:.3g}] back in {:.3g} M (at t {:.3g})",
                    static_cast<int>(director_.stage()), director_.pace(), director_.diskPresence(), director_.cameraEngaged(),
                    gs.inPlay, gs.nearHole, gs.lo.x, gs.lo.y, gs.lo.z, gs.hi.x, gs.hi.y, gs.hi.z, gs.returnInM, gs.timeM);
            logInfo("tde t={:.3g} M stage {} star r={:.3g} M intact={} released={} watch={} cam r={:.3g} M to star {:.3g} M volumes={} world warp={:.3g} event warp={:.3g}",
                    st.timeM, static_cast<int>(director_.stage()), glm::length(tde_.starPosition()), tde_.starIntact(), st.released, director_.cameraEngaged(),
                    glm::length(camera_.position()), glm::length(camera_.position() - tde_.starPosition()), tde_.volumeCount(),
                    settings_.timeWarp, tdeWarp_);
            {
                const glm::dvec3 rel = tde_.starPosition() - camera_.position();
                const glm::dvec3 back(camera_.basis()[2]);
                logInfo("    debris seen at {:.1f} deg from the view axis (camera {:.3g} M from it)",
                        glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(rel), -back), -1.0, 1.0))), glm::length(rel));
            }
            std::string vols;
            for (int i = 0; i < tde_.volumeCount(); ++i)
                vols += std::format(" [{:.3g} M at r {:.3g}]", tde_.volume(i).halfSize, glm::length(glm::dvec3(tde_.volume(i).center)));
            logInfo("    active {} bound {:.2f} accreted {:.2f} escaped {:.2f} heating {:.3g} limited {} volumes{}", st.activeParticles,
                    st.boundFraction, st.accretedFraction, st.escapedFraction, st.dissipationRel, st.limited, vols);
            for (int i = 0; i < tde_.volumeCount(); ++i) {   // what the volumes hold (debug only: a read-back stall)
                const tde::GasVolume& gv = tde_.volume(i);
                const int n = gv.resolution;
                std::vector<float> px(static_cast<size_t>(n) * n * n * 4);
                glBindTexture(GL_TEXTURE_3D, gv.texture);
                glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_FLOAT, px.data());
                glBindTexture(GL_TEXTURE_3D, 0);
                float maxK = 0, maxT = 0, maxB = 0;
                size_t filled = 0, emitting = 0;
                double sumB = 0;
                std::vector<float> ks;
                for (size_t t = 0; t < px.size(); t += 4) {
                    if (px[t] > 0.0f) {
                        ++filled;
                        ks.push_back(px[t]);
                    }
                    if (px[t + 2] > 0.0f) { ++emitting; sumB += px[t + 2]; }
                    maxK = std::max(maxK, px[t]);
                    maxT = std::max(maxT, px[t + 1]);
                    maxB = std::max(maxB, px[t + 2]);
                }
                std::sort(ks.begin(), ks.end());
                const auto pct = [&](double q) { return ks.empty() ? 0.0f : ks[static_cast<size_t>(q * (ks.size() - 1))]; };
                const float texel = 2.0f * gv.halfSize / static_cast<float>(n);
                logInfo("    volume {}: filled {} emitting {} k per M p10 {:.3g} p50 {:.3g} p90 {:.3g} max {:.3g} (tau per texel at p50 {:.3g}) max T {:.3g}e4 K max b {:.3g} mean b {:.3g}",
                        i, filled, emitting, pct(0.1), pct(0.5), pct(0.9), maxK, pct(0.5) * texel, maxT, maxB, emitting ? sumB / emitting : 0.0);
            }
        }
    }
    // Star disruption camera: the director's viewpoint for each stage, until the user flies the camera (or
    // starts a flight somewhere); Watch hands it back.
    if (director_.cameraEngaged()) {
        const bool steering = glm::length(in.move) > 0.0f || glm::length(in.lookDelta) > 0.0f || in.roll != 0.0f;
        if (steering || camera_.isTransitioning()) {
            director_.releaseCamera();
        } else {
            glm::dvec3 pos, target;
            director_.view(pos, target);
            camera_.setPose(pos, camera_.orientation());
            camera_.lookAt(target);
            if (std::getenv("BHS_DEBUG_CAM")) {   // test hook: where the director's camera really ends up
                const glm::dvec3 star = tde_.starPosition();
                const glm::dvec3 rel = star - camera_.position();
                const glm::dvec3 fwd = -glm::dvec3(camera_.basis()[2]);
                logInfo("cam f{} want {:.3f} {:.3f} {:.3f} got {:.3f} {:.3f} {:.3f} target {:.2f} {:.2f} {:.2f} star {:.3f} {:.3f} {:.3f} dist {:.3f} angle {:.1f} deg",
                        frameIndex_, pos.x, pos.y, pos.z, camera_.position().x, camera_.position().y, camera_.position().z,
                        target.x, target.y, target.z, star.x, star.y, star.z, glm::length(rel),
                        glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(rel), fwd), -1.0, 1.0))));
            }
        }
    }

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
