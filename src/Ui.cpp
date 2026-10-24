#include "Ui.h"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>

#include "Camera.h"
#include "Renderer.h"
#include "Settings.h"

namespace bhs {

bool Ui::init(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write imgui.ini
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.82f;
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 150")) return false;
    inited_ = true;
    return true;
}

void Ui::shutdown() {
    if (!inited_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    inited_ = false;
}

void Ui::newFrame(bool mouseCaptured) {
    ImGuiIO& io = ImGui::GetIO();
    // While the cursor is disabled GLFW keeps feeding ImGui an unbounded virtual cursor; ignore it.
    if (mouseCaptured) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    // Drop widget keyboard focus so WantCaptureKeyboard cannot freeze flight controls.
    if (mouseCaptured) ImGui::SetWindowFocus(nullptr);
}

void Ui::endFrame() { ImGui::EndFrame(); }

void Ui::render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool Ui::wantsKeyboard() const { return inited_ && ImGui::GetIO().WantCaptureKeyboard; }
bool Ui::wantsMouse() const { return inited_ && ImGui::GetIO().WantCaptureMouse; }
bool Ui::anyItemActive() const { return inited_ && ImGui::IsAnyItemActive(); }

namespace {
bool sliderLog(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f") {
    return ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_Logarithmic);
}
void markCustom(Settings& s, bool changed) {
    if (changed) s.preset = PRESET_CUSTOM;
}
}  // namespace

void Ui::draw(UiContext& c) {
    Settings& s = *c.settings;
    Camera& cam = *c.camera;
    Renderer& r = *c.renderer;

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Black Hole Simulation", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // ---- stats ----
    ImGui::TextUnformatted(c.gpuName);
    if (c.bridged) {
        ImGui::SameLine();
        ImGui::TextDisabled("(shown via the iGPU's Wayland window)");
    }
    if (!c.isNvidia && c.nvidiaDriverLoaded) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Integrated GPU in use. Start with ./run.sh (without --igpu) for the RTX 3060.");
    } else if (!c.isNvidia) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Not running on the RTX 3060. Enroll the driver's Secure Boot key (README, 'Enabling the RTX 3060').");
    }
    const glm::ivec2 isz = r.internalSize();
    ImGui::Text("%.0f fps   GPU %.2f ms   %dx%d (%.0f%%)   accum %d", c.fps, r.gpuMs(), isz.x, isz.y,
                100.0f * s.renderScale * r.dynamicScale(), r.accumulatedFrames());
    ImGui::Text("r = %.2f M   speed %.2f M/s%s", cam.radius(), cam.speedScale(), cam.autoOrbit() ? "   [auto-orbit]" : "");
    if (!r.shadersOk()) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Shader errors: see terminal (F5 to retry)");
    ImGui::TextDisabled(c.mouseCaptured ? "Esc / Tab: release mouse to use the panel" : "Tab or click: capture mouse to fly   Esc: quit");

    // ---- quality ----
    if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* names[] = {"Low", "Medium", "High", "Ultra", "Custom"};
        int p = s.preset;
        if (ImGui::Combo("Preset", &p, names, 5) && p != PRESET_CUSTOM) s.applyPreset(p);
        markCustom(s, ImGui::SliderFloat("Render scale", &s.renderScale, 0.25f, 1.0f, "%.2f"));
        markCustom(s, ImGui::SliderInt("Max steps", &s.maxSteps, 50, 2000));
        markCustom(s, sliderLog("Step scale", &s.stepScale, 0.01f, 0.25f));
        markCustom(s, sliderLog("Disk step", &s.diskStep, 0.03f, 1.0f));
        const char* dm[] = {"None", "Thin", "Volumetric"};
        markCustom(s, ImGui::Combo("Disk mode", &s.diskMode, dm, 3));
        const char* res[] = {"1024", "2048", "4096"};
        int ri = s.skyResolution >= 4096 ? 2 : (s.skyResolution >= 2048 ? 1 : 0);
        if (ImGui::Combo("Sky resolution", &ri, res, 3)) {
            s.skyResolution = ri == 2 ? 4096 : (ri == 1 ? 2048 : 1024);
            s.preset = PRESET_CUSTOM;
        }
        ImGui::Checkbox("Dynamic resolution", &s.dynamicResolution);
        if (s.dynamicResolution) ImGui::SliderFloat("Target ms", &s.targetFrameMs, 8.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Accumulate when still", &s.accumulate);
        ImGui::SameLine();
        ImGui::Checkbox("VSync", &s.vsync);
    }

    // ---- physics ----
    if (ImGui::CollapsingHeader("Physics")) {
        ImGui::Checkbox("Gravitational lensing (L)", &s.lensing);
        ImGui::SliderFloat("Doppler + redshift", &s.dopplerStrength, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("0 = Interstellar look, 1 = physical beaming/redshift");
        ImGui::Checkbox("Shadow edge overlay", &s.showShadowOverlay);
        const char* dv[] = {"Off", "Step count", "Footprint"};
        ImGui::Combo("Debug view", c.debugView, dv, 3);
    }

    // ---- disk ----
    if (ImGui::CollapsingHeader("Accretion disk")) {
        ImGui::SliderFloat("Inner radius", &s.diskInner, 4.0f, 12.0f, "%.1f M");
        ImGui::SliderFloat("Outer radius", &s.diskOuter, 8.0f, 60.0f, "%.1f M");
        s.diskOuter = std::max(s.diskOuter, s.diskInner + 1.0f);
        sliderLog("Thickness (H/r)", &s.diskThickness, 0.005f, 0.4f);
        sliderLog("Density", &s.diskDensity, 0.05f, 10.0f);
        sliderLog("Opacity", &s.diskOpacity, 0.05f, 20.0f);
        sliderLog("Peak temperature", &s.diskPeakTemp, 2000.0f, 30000.0f, "%.0f K");
        sliderLog("Brightness", &s.diskBrightness, 0.05f, 20.0f);
        ImGui::SliderFloat("Noise scale", &s.diskNoiseScale, 0.25f, 4.0f, "%.2f");
        ImGui::SliderFloat("Noise strength", &s.diskNoiseStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Rotation speed", &s.diskSpeed, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Warm tint", &s.diskWarmth, 0.0f, 1.0f, "%.2f");
        ImGui::Checkbox("Pause disk (P)", &s.paused);
    }

    // ---- sky ----
    if (ImGui::CollapsingHeader("Sky")) {
        int seed = static_cast<int>(s.skySeed);
        if (ImGui::InputInt("Seed", &seed)) s.skySeed = static_cast<uint32_t>(std::max(0, seed));
        ImGui::SameLine();
        if (ImGui::Button("Random")) s.skySeed = static_cast<uint32_t>(std::rand());
        ImGui::SliderInt("Galaxies", &s.galaxyCount, 0, 12000);
        if (ImGui::Button("Regenerate sky")) *c.requestRebake = true;
        ImGui::SameLine();
        ImGui::TextDisabled("last bake %.0f ms", c.skyBakeMs);
        if (c.skyDirty) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Sky rebakes when you release the control (~%.0f ms)", c.skyBakeMs);
        sliderLog("Sky brightness", &s.skyBrightness, 0.05f, 10.0f);
        sliderLog("Galaxy brightness", &s.galaxyBrightness, 0.05f, 10.0f);
        sliderLog("Milky Way", &s.milkyWayBrightness, 0.0f + 0.01f, 10.0f);
        sliderLog("Nebulae", &s.nebulaBrightness, 0.01f, 10.0f);
        ImGui::SliderFloat("Star density", &s.starDensity, 0.0f, 2.5f, "%.2f");
        sliderLog("Star brightness", &s.starBrightness, 0.05f, 10.0f);
    }

    // ---- post ----
    if (ImGui::CollapsingHeader("Post-processing")) {
        sliderLog("Exposure", &s.exposure, 0.05f, 20.0f);
        ImGui::Checkbox("Auto exposure", &s.autoExposure);
        ImGui::SliderFloat("Bloom", &s.bloomIntensity, 0.0f, 1.5f, "%.2f");
        sliderLog("Bloom threshold", &s.bloomThreshold, 0.1f, 10.0f);
        const char* tm[] = {"AgX", "ACES", "Reinhard", "None"};
        ImGui::Combo("Tonemap", &s.tonemap, tm, 4);
        ImGui::SliderFloat("Vignette", &s.vignette, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Film grain", &s.grain, 0.0f, 0.15f, "%.3f");
        ImGui::SliderFloat("Chromatic aberration", &s.chromaticAberration, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Sharpen", &s.sharpen, 0.0f, 1.0f, "%.2f");
    }

    // ---- camera ----
    if (ImGui::CollapsingHeader("Camera")) {
        ImGui::SliderFloat("Field of view", &s.fovDeg, 30.0f, 120.0f, "%.0f deg");
        ImGui::SliderFloat("Mouse sensitivity", &s.mouseSensitivity, 0.02f, 0.6f, "%.2f");
        ImGui::Checkbox("Invert Y", &s.invertY);
        sliderLog("Speed multiplier", &cam.baseSpeed, 0.05f, 50.0f);
        bool orbit = cam.autoOrbit();
        if (ImGui::Checkbox("Auto-orbit (O)", &orbit)) cam.setAutoOrbit(orbit);
        if (orbit) ImGui::SliderFloat("Orbit speed", &cam.autoOrbitSpeed, -0.5f, 0.5f, "%.2f rad/s");
        ImGui::Text("Bookmarks:");
        const auto& bms = Camera::defaultBookmarks();
        for (size_t i = 0; i < bms.size(); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(bms[i].name.c_str())) cam.flyTo(bms[i]);
        }
        const glm::vec3 p = cam.position();
        ImGui::Text("pos (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    }

    // ---- capture / misc ----
    if (ImGui::CollapsingHeader("Capture & shaders")) {
        if (ImGui::Button("Screenshot (F2)")) *c.requestScreenshot = true;
        ImGui::SameLine();
        if (ImGui::Button("Cinematic 4K (F3)")) *c.requestCinematic = true;
        if (ImGui::Button("Reload shaders (F5)")) *c.requestReload = true;
        ImGui::SameLine();
        if (ImGui::Button("Save settings")) *c.requestSave = true;
    }

    if (ImGui::CollapsingHeader("Controls", showHelp_ ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::BulletText("Mouse: look      W/A/S/D: move      Space / Ctrl: up / down");
        ImGui::BulletText("Q / E: roll      Shift: 5x boost      Scroll: speed");
        ImGui::BulletText("1-4: bookmarks   R: reset view   O: auto-orbit   L: toggle lensing");
        ImGui::BulletText("Tab: mouse capture   H: hide panel   P: pause disk   Esc: release mouse / quit");
        ImGui::BulletText("F2: screenshot   F3: cinematic capture   F5: reload shaders");
    }
    ImGui::End();
}

}  // namespace bhs
