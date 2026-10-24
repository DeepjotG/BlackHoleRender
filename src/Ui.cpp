#include "Ui.h"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "BlackHoleSystem.h"
#include "Bodies.h"
#include "Camera.h"
#include "Units.h"
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
bool sliderLogD(const char* label, double* v, double lo, double hi, const char* fmt) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, v, &lo, &hi, fmt, ImGuiSliderFlags_Logarithmic);
}
const char* lookName(universe::PlanetLook l) {
    switch (l) {
        case universe::PlanetLook::Rocky: return "rocky world";
        case universe::PlanetLook::Desert: return "desert world";
        case universe::PlanetLook::Ocean: return "ocean world";
        case universe::PlanetLook::Ice: return "ice world";
        case universe::PlanetLook::Earthlike: return "Earth-like world";
        case universe::PlanetLook::GasGiantWarm: return "gas giant";
        case universe::PlanetLook::GasGiantCold: return "ice giant";
        case universe::PlanetLook::Cloudy: return "cloud-covered world";
        default: return "lava world";
    }
}
}  // namespace

void Ui::drawMarkers(UiContext& c) {
    const Settings& s = *c.settings;
    const Camera& cam = *c.camera;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 0.0f || ds.y <= 0.0f) return;
    const float aspect = ds.x / ds.y;
    const float tanHalf = std::tan(glm::radians(s.fovDeg) * 0.5f);
    const glm::mat3 bt = glm::transpose(cam.basis());
    const double mPerM = units::metersPerM(s.bhMassSolar);
    auto project = [&](const glm::dvec3& p, ImVec2& out, double& dist) {
        const glm::dvec3 rel = p - cam.position();
        dist = glm::length(rel);
        if (dist <= 0.0) return false;
        const glm::vec3 v = bt * glm::vec3(rel / dist);
        if (v.z > -0.02f) return false;
        const float sx = v.x / (-v.z) / (tanHalf * aspect);
        const float sy = v.y / (-v.z) / tanHalf;
        out = ImVec2((sx * 0.5f + 0.5f) * ds.x, (0.5f - sy * 0.5f) * ds.y);
        return out.x > -50.0f && out.y > -50.0f && out.x < ds.x + 50.0f && out.y < ds.y + 50.0f;
    };
    auto label = [&](const ImVec2& at, float radiusPx, const std::string& name, double distM, ImU32 col) {
        const float rr = std::max(radiusPx * 1.25f, 7.0f);
        dl->AddCircle(at, rr, col, 32, 1.5f);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s  %s", name.c_str(), units::formatDistance(distM * mPerM).c_str());
        dl->AddText(ImVec2(at.x + rr + 4.0f, at.y - 7.0f), col, buf);
    };
    if (c.bodies) {
        for (const BodyDesc& b : *c.bodies) {
            ImVec2 at;
            double dist;
            if (!project(b.pos, at, dist)) continue;
            const float rpx = static_cast<float>(std::atan(b.radius / dist) / tanHalf * 0.5 * ds.y);
            if (rpx > ds.y * 0.25f) continue;   // filling the view: no marker needed
            label(at, rpx, b.name, std::max(dist - b.radius, 0.0), IM_COL32(150, 220, 255, 220));
        }
    }
    if (cam.radius() > 60.0) {
        ImVec2 at;
        double dist;
        if (project(glm::dvec3(0.0), at, dist)) label(at, 0.0f, "black hole", std::max(dist - 2.0, 0.0), IM_COL32(255, 200, 140, 200));
    }
}

void Ui::draw(UiContext& c) {
    Settings& s = *c.settings;
    if (s.showLabels) drawMarkers(c);
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
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s",
                           c.igpuForced ? "Integrated GPU in use (--igpu). Start without --igpu for the RTX 3060."
                                        : "RTX 3060 unavailable: check nvidia-smi (reboot after a driver update). Details in the terminal.");
    } else if (!c.isNvidia) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Not running on the RTX 3060. Enroll the driver's Secure Boot key (README, 'Enabling the RTX 3060').");
    }
    const glm::ivec2 isz = r.internalSize();
    ImGui::Text("%.0f fps   GPU %.2f ms   %dx%d (%.0f%%)   accum %d", c.fps, r.gpuMs(), isz.x, isz.y,
                100.0f * s.renderScale * r.dynamicScale(), r.accumulatedFrames());
    {
        const double mPerM = units::metersPerM(s.bhMassSolar);
        const std::string d = units::formatDistance(cam.radius() * mPerM);
        const std::string near = units::formatDistance(std::max(c.nearestDist, 0.0) * mPerM);
        const std::string v = units::formatSpeed(cam.lastSpeed() * mPerM);
        ImGui::Text("%.4g M from the hole (%s)   nearest surface: %s, %s", cam.radius(), d.c_str(), c.nearestName, near.c_str());
        ImGui::Text("flight speed %s%s%s", v.c_str(), cam.autoOrbit() ? "   [auto-orbit]" : "",
                    (c.followName && !c.followName->empty()) ? "   [riding along]" : "");
    }
    if (!r.shadersOk()) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Shader errors: see terminal (F5 to retry)");
    ImGui::TextDisabled(c.mouseCaptured ? "Esc / Tab: release mouse to use the panel" : "Tab or click: capture mouse to fly   Esc: quit");

    // ---- black hole and time ----
    if (ImGui::CollapsingHeader("Black hole & time", ImGuiTreeNodeFlags_DefaultOpen)) {
        const double mPerM = units::metersPerM(s.bhMassSolar);
        const double sPerM = units::secondsPerM(s.bhMassSolar);
        sliderLogD("Mass (suns)", &s.bhMassSolar, 3.0, 1e10, "%.3g");
        if (ImGui::SmallButton("Stellar (10)")) s.bhMassSolar = 10.0;
        ImGui::SameLine();
        if (ImGui::SmallButton("Sgr A* (4.3 M)")) s.bhMassSolar = 4.297e6;
        ImGui::SameLine();
        if (ImGui::SmallButton("Gargantua (100 M)")) s.bhMassSolar = 1e8;
        ImGui::SameLine();
        if (ImGui::SmallButton("M87* (6.5 B)")) s.bhMassSolar = 6.5e9;
        ImGui::TextDisabled("%s; horizon radius %s; 1 M = %s", units::formatMassSolar(s.bhMassSolar).c_str(),
                            units::formatDistance(2.0 * mPerM).c_str(), units::formatDistance(mPerM).c_str());
        sliderLogD("Time warp", &s.timeWarp, 0.01, 1e11, "%.3g x");
        const struct { const char* label; double v; } warps[] = {
            {"Real time", 1.0}, {"1 min/s", 60.0}, {"1 h/s", 3600.0}, {"1 day/s", units::kDay}, {"1 yr/s", units::kYear}, {"1000 yr/s", 1000.0 * units::kYear}};
        for (size_t i = 0; i < sizeof(warps) / sizeof(warps[0]); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(warps[i].label)) s.timeWarp = warps[i].v;
        }
        ImGui::Text("1 s here = %s   elapsed %s%s", units::formatDuration(s.timeWarp).c_str(),
                    units::formatDuration(c.simSeconds).c_str(), s.paused ? "   [paused, P]" : "");
        const double rc = cam.radius();
        ImGui::TextDisabled("your clock runs %s slower than a far-away clock (hovering at %.3g M)",
                            units::formatFactor(BlackHoleSystem::timeDilationStatic(rc)).c_str(), rc);
        (void)sPerM;
    }

    // ---- planets ----
    if (c.bhSystem && ImGui::CollapsingHeader("Planets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show planets", &s.planetsEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Labels", &s.showLabels);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) c.bhSystem->resetPlanets();
        if (s.planetsEnabled) {
            const double mPerM = units::metersPerM(s.bhMassSolar);
            const double sPerM = units::secondsPerM(s.bhMassSolar);
            for (size_t i = 0; i < c.bhSystem->planets.size(); ++i) {
                Planet& p = c.bhSystem->planets[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Separator();
                ImGui::Text("%s, %s", p.name.c_str(), lookName(p.look));
                ImGui::SameLine();
                if (ImGui::SmallButton("Go")) *c.goToRequest = p.name;
                ImGui::SameLine();
                const bool riding = c.followName && *c.followName == p.name;
                if (ImGui::SmallButton(riding ? "Stop riding" : "Ride along")) *c.followName = riding ? std::string() : p.name;
                sliderLogD("Orbit radius (M)", &p.orbitRadiusM, 3.05, 5000.0, "%.4g");
                const double tiltMin = -90.0, tiltMax = 90.0;
                ImGui::SliderScalar("Tilt (deg)", ImGuiDataType_Double, &p.inclinationDeg, &tiltMin, &tiltMax, "%.1f");
                const double period = BlackHoleSystem::orbitalPeriodM(p.orbitRadiusM) * sPerM;
                const double dil = BlackHoleSystem::timeDilationCircularOrbit(p.orbitRadiusM);
                ImGui::TextDisabled("orbit %s, year %s, speed %.3g c", units::formatDistance(p.orbitRadiusM * mPerM).c_str(),
                                    units::formatDuration(period).c_str(), BlackHoleSystem::orbitalSpeedLocal(p.orbitRadiusM));
                ImGui::TextDisabled("1 hour on %s = %s far away (%s)", p.name.c_str(), units::formatDuration(3600.0 * dil).c_str(),
                                    units::formatFactor(dil).c_str());
                if (p.orbitRadiusM < 6.0)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Inside the innermost stable orbit (6 M): it would spiral in.");
                const double roche = BlackHoleSystem::rocheLimitM(p, s.bhMassSolar);
                if (p.orbitRadiusM < roche)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Tides would tear it apart here (needs r > %.3g M at this mass).", roche);
                ImGui::PopID();
            }
            ImGui::TextDisabled("The film's '1 hour = 7 years' needs a fast-spinning black hole; around a non-spinning one");
            ImGui::TextDisabled("a stable orbit slows time by at most 1.41x.");
        }
    }

    // ---- travel ----
    if (ImGui::CollapsingHeader("Travel", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Black hole")) *c.goToRequest = "black hole";
        if (c.bodies) {
            for (const BodyDesc& b : *c.bodies) {
                ImGui::SameLine();
                if (ImGui::Button(b.name.c_str())) *c.goToRequest = b.name;
            }
        }
        if (c.followName && !c.followName->empty()) {
            ImGui::Text("Riding along with %s", c.followName->c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) c.followName->clear();
        }
        ImGui::TextDisabled("Speed follows the distance to the nearest surface; scroll to change it, Shift = 5x.");
    }

    // ---- quality ----
    if (ImGui::CollapsingHeader("Quality")) {
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
        sliderLog("Speed multiplier", &cam.baseSpeed, 1e-4f, 1e9f, "%.3g");
        bool orbit = cam.autoOrbit();
        if (ImGui::Checkbox("Auto-orbit (O)", &orbit)) cam.setAutoOrbit(orbit);
        if (orbit) ImGui::SliderFloat("Orbit speed", &cam.autoOrbitSpeed, -0.5f, 0.5f, "%.2f rad/s");
        ImGui::Text("Bookmarks:");
        const auto& bms = Camera::defaultBookmarks();
        for (size_t i = 0; i < bms.size(); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(bms[i].name.c_str())) cam.flyTo(bms[i]);
        }
        const glm::dvec3 p = cam.position();
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
