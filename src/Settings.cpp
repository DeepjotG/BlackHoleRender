#include "Settings.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace bhs {

void Settings::applyPreset(int p) {
    preset = p;
    switch (p) {
        case PRESET_LOW:
            renderScale = 0.5f; maxSteps = 200; stepScale = 0.10f; diskStep = 0.35f;
            diskMode = DISK_THIN; skyResolution = 2048;
            break;
        case PRESET_MEDIUM:
            renderScale = 0.67f; maxSteps = 300; stepScale = 0.07f; diskStep = 0.30f;
            diskMode = DISK_VOLUMETRIC; skyResolution = 2048;
            break;
        case PRESET_HIGH:
            renderScale = 0.85f; maxSteps = 400; stepScale = 0.05f; diskStep = 0.20f;
            diskMode = DISK_VOLUMETRIC; skyResolution = 2048;
            break;
        case PRESET_ULTRA:
            renderScale = 1.0f; maxSteps = 600; stepScale = 0.035f; diskStep = 0.12f;
            diskMode = DISK_VOLUMETRIC; skyResolution = 4096;
            break;
        default:
            preset = PRESET_CUSTOM;
            break;
    }
}

const char* Settings::presetName(int p) {
    switch (p) {
        case PRESET_LOW: return "Low";
        case PRESET_MEDIUM: return "Medium";
        case PRESET_HIGH: return "High";
        case PRESET_ULTRA: return "Ultra";
        default: return "Custom";
    }
}

namespace {

struct Field {
    const char* name;
    std::function<std::string(const Settings&)> get;
    std::function<void(Settings&, const std::string&)> set;
};

template <class T>
std::string toStr(const T& v) { return std::to_string(v); }
std::string toStr(bool v) { return v ? "1" : "0"; }
std::string toStr(float v) { char buf[64]; std::snprintf(buf, sizeof buf, "%.6g", v); return buf; }

void fromStr(const std::string& s, float& v) { v = std::strtof(s.c_str(), nullptr); }
void fromStr(const std::string& s, int& v) { v = static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }
void fromStr(const std::string& s, uint32_t& v) { v = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }
void fromStr(const std::string& s, bool& v) { v = (s == "1" || s == "true"); }

#define BHS_FIELD(member)                                                                     \
    Field{#member, [](const Settings& s) { return toStr(s.member); },                         \
          [](Settings& s, const std::string& v) { fromStr(v, s.member); }}

const std::vector<Field>& fields() {
    static const std::vector<Field> f = {
        BHS_FIELD(preset), BHS_FIELD(renderScale), BHS_FIELD(maxSteps), BHS_FIELD(stepScale),
        BHS_FIELD(diskStep), BHS_FIELD(diskMode), BHS_FIELD(skyResolution), BHS_FIELD(dynamicResolution),
        BHS_FIELD(targetFrameMs), BHS_FIELD(accumulate), BHS_FIELD(maxAccumFrames), BHS_FIELD(vsync),
        BHS_FIELD(lensing), BHS_FIELD(dopplerStrength),
        BHS_FIELD(diskInner), BHS_FIELD(diskOuter), BHS_FIELD(diskThickness), BHS_FIELD(diskDensity),
        BHS_FIELD(diskOpacity), BHS_FIELD(diskPeakTemp), BHS_FIELD(diskBrightness), BHS_FIELD(diskNoiseScale),
        BHS_FIELD(diskNoiseStrength), BHS_FIELD(diskSpeed),
        BHS_FIELD(skySeed), BHS_FIELD(galaxyCount), BHS_FIELD(galaxyBrightness), BHS_FIELD(milkyWayBrightness),
        BHS_FIELD(nebulaBrightness), BHS_FIELD(starDensity), BHS_FIELD(starBrightness), BHS_FIELD(skyBrightness),
        BHS_FIELD(exposure), BHS_FIELD(autoExposure), BHS_FIELD(bloomIntensity), BHS_FIELD(bloomThreshold),
        BHS_FIELD(tonemap), BHS_FIELD(vignette), BHS_FIELD(grain), BHS_FIELD(chromaticAberration),
        BHS_FIELD(saturation), BHS_FIELD(sharpen),
        BHS_FIELD(fovDeg), BHS_FIELD(mouseSensitivity), BHS_FIELD(invertY),
        BHS_FIELD(showShadowOverlay), BHS_FIELD(showUi),
    };
    return f;
}
#undef BHS_FIELD

}  // namespace

bool Settings::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# BlackholeSimulation settings\n";
    for (const auto& f : fields()) out << f.name << "=" << f.get(*this) << "\n";
    return true;
}

std::string Settings::fingerprint() const {
    std::string out;
    out.reserve(1024);
    for (const auto& f : fields()) {
        out += f.get(*this);
        out += ';';
    }
    out += paused ? '1' : '0';
    return out;
}

bool Settings::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        for (const auto& f : fields()) {
            if (key == f.name) {
                f.set(*this, val);
                break;
            }
        }
    }
    return true;
}

}  // namespace bhs
